"""The live engine's trading day: is it well (check), what happened (report), and did paper trade as
a backtest of the same day would have (compare).

A scheduler drives it through the API (POST /api/session/check, POST /api/session/report; the
systemd timers kept with the machine's deployment, outside this repository). With notify, a message goes out through
$OPTRA_NOTIFY (default `notify`, its one argument the text): for check only when what is wrong
changes, for report a summary.
"""

from __future__ import annotations

import json
import os
import shlex
import subprocess
from collections import defaultdict
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Any

from optra.research import code_identity, evidence, jobs, ledger, promotion, runner, strategies
from optra.data import daily_values
from optra.data.days import ET, parse_option
from optra.infra.remote_client import EngineClient
from optra.utils import files



PNL_SOURCES = ("delta", "gamma", "theta", "vega", "other", "fills")  # the holding summary's pnl_*

def notify(text: str) -> None:
    command = shlex.split(os.environ.get("OPTRA_NOTIFY", "notify"))
    try:
        subprocess.run([*command, text], timeout=60, check=False)
    except OSError as exc:
        print(f"notify: {exc}")


def _data() -> Path:
    return files.data_root()


# ── check ─────────────────────────────────────────────────────────────────────


def problems(client: EngineClient) -> list[str]:
    """What is wrong with the engine now; empty: nothing."""
    try:
        status = client.get_status()
    except Exception as exc:  # gRPC: the engine is down
        return [f"the engine does not answer ({_code(exc)})"]
    out = []
    if not status["connected"]:
        out.append("not connected to IB")
    if "md: on" not in status["detail"]:
        out.append("market data off")
    try:
        risk = client.get_risk()
        if risk["halted"]:
            out.append(f"trading halted: {risk['halt_reason']}")
        out += [f"only closing orders today: {why}" for why in risk["day_stops"].values()]
    except Exception as exc:
        out.append(f"no risk state ({_code(exc)})")
    try:
        client.check_with_broker()  # the check strategies start on, recorded; reading the account changes nothing
    except Exception as exc:
        out.append(f"no broker check ({_code(exc)})")
    try:
        account = client.get_account()
        if account["broker_check"] not in ("agree", ""):
            out.append(f"broker check: {account['broker_check']} "
                       f"{account['broker_check_problem']}".strip())
        for name, options in account["unsettled"].items():
            out.append(f"{name}: expired options not settled ({', '.join(options)})")
    except Exception as exc:
        out.append(f"no account from IB ({_code(exc)})")
    try:
        for strategy in client.list_strategies():
            if strategy["status"] == "error":
                out.append(f"{strategy['strategy_name']} in error")
    except Exception as exc:
        out.append(f"no strategies ({_code(exc)})")
    return out


def _named(account: str) -> str:
    """How a message names the account's engine."""
    return "Optra" if account == "paper" else f"Optra {account}"


def check(client: EngineClient, send: bool, account: str = "paper") -> dict[str, Any]:
    """problems() of the account's engine; with send, a message when the notified problems change (state kept in
    data/session-check.json, data/session-check-<account>.json for another). A problem is notified once it is seen by
    two checks in a row: the
    engine does not answer for about a minute while it starts (the morning restart, a deploy), and
    a crash is notified separately (optra-alert@). Recovery is notified for the problems that
    were notified, naming them. Moves of versions between stages under way are carried out again
    first (promotion.advance_pending, with the paper account's check); one still stopped is a problem."""
    now = (promotion.advance_pending() if account == "paper" else []) + problems(client)
    state = _data() / ("session-check.json" if account == "paper" else f"session-check-{account}.json")
    try:
        kept = json.loads(state.read_text())
    except (OSError, ValueError):
        kept = {}
    if isinstance(kept, list):  # the earlier format: the last check's problems, all notified
        kept = {"seen": kept, "notified": kept}
    seen, notified = kept.get("seen", []), kept.get("notified", [])
    persistent = [p for p in now if p in seen]
    message = ""
    if persistent and persistent != notified:
        message = f"{_named(account)}: " + "; ".join(persistent)
    elif not persistent and notified and not now:
        message = f"{_named(account)}: back to normal; was: " + "; ".join(notified)
    if persistent or not now:
        notified = persistent
    if now != seen or notified != kept.get("notified", []):
        state.parent.mkdir(parents=True, exist_ok=True)
        state.write_text(json.dumps({"seen": now, "notified": notified}))
    if send and message:
        notify(message)
    return {"status": "ok", "problems": now, "notified": notified, "message": message}


# ── fills ─────────────────────────────────────────────────────────────────────


def _code(exc: Exception) -> str:
    code = getattr(exc, "code", None)
    return code().name if callable(code) else type(exc).__name__


def _when(record: dict) -> datetime:
    text = record.get("datetime") or record["timestamp"]
    return datetime.strptime(text, "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)


def paper_fills(client: EngineClient, day: date, strategy: str | None = None) -> dict[str, list[dict]]:
    """Each strategy's fills on the New York date, in the order they were saved."""
    records = client.get_orders_and_fills(strategy, 0)["records"]
    out: dict[str, list[dict]] = {}
    for r in sorted((r for r in records if r["record_type"] == "Fill"), key=lambda r: r["timestamp"]):
        at = _when(r)
        if at.astimezone(ET).date() == day:
            out.setdefault(r["strategy_name"], []).append(
                {"symbol": r["symbol"], "side": r["side"], "price": r["price"],
                 "quantity": r["quantity"], "time": at})
    return out


# ── compare: a paper day against a backtest of it ────────────────────────────


def _multiplier(symbol: str) -> float:
    return 100.0 if parse_option(symbol) else 1.0


def _cash(fills: list[dict]) -> float:
    """What the fills paid (negative) and received: a day flat at the end, its P&L."""
    return sum((1 if f["side"] == "SELL" else -1) * f["price"] * f["quantity"]
               * _multiplier(f["symbol"]) for f in fills)


def _clock(t: datetime | None) -> str:
    return t.astimezone(ET).strftime("%H:%M:%S") if t else "-"


def backtest_fills(data: Path, portfolio: str, cls: str, setting: dict[str, float],
                   costs: ledger.Costs, day: date) -> tuple[list[dict], float]:
    """The strategy's fills in a backtest of the recorded day (a job: research.jobs), and what its orders paid over
    the mids (its round trips' entry and exit costs)."""
    file = data / portfolio / f"{portfolio}-{day:%Y-%m}" / f"{day:%Y%m%d}.parquet"
    if not file.exists():
        raise ValueError(f"no recorded day {file} (scripts/data/record_quotes.py records them)")
    trial = {"id": cls, "strategy": cls, "setting": setting, "costs": costs.model_dump(), "fills": True}
    result = jobs.run({"files": [str(file)], "risk_free_rate": 0.05, "iv_price_mode": "mid", "trials": [trial]},
                      jobs.PRIORITY_REPORT)[1][0]
    if result.get("errors"):
        raise ValueError(f"entry_backtest: {result['errors']}")
    fills = []
    for t in runner.rows(result["fills"]):
        at = datetime.strptime(t["time"], "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc) if t["time"] else None
        fills.append({"symbol": t["symbol"], "side": t["side"], "price": t["price"], "quantity": t["quantity"],
                      "time": at})
    trips = result.get("round_trips") or {}
    paid = sum(c or 0.0 for c in trips.get("entry_cost") or []) + sum(c or 0.0 for c in trips.get("exit_cost") or [])
    return fills, paid


def compare(client: EngineClient, strategy: str, day: date, fee_rate: float = 0.65,
            data: Path | None = None) -> dict[str, Any]:
    """A paper day against a backtest of the same recorded day: the strategy's class, its setting
    and the risk limits as the live engine has them, over the day's recorded quotes (data/
    recorded), at the costs its version was found at (fee_rate's otherwise). Each contract
    and side in time order, the n-th paper fill against the n-th backtest one; what only one side
    has; each side's cash flow; what the backtest's fills paid over the mids, and the paper's: the
    same less the matched fills' price differences.

    Differences to expect: IB's paper fills are not the backtest's (it fills each leg at the bid or
    ask of the minute's quotes); recorded quotes are the minute's last, and delayed ones
    when the paper account has no real-time data."""
    record = next((s for s in client.list_strategies() if s["strategy_name"] == strategy), None)
    if record is None:
        raise ValueError(f"no strategy {strategy}")
    listed = strategies.strategy_listing().get(record["class_name"], {})
    research = Path(listed.get("library") or "")
    installed = strategies.installed_library("paper", Path(listed.get("source") or "x").name)
    if research.is_file() and installed.is_file() and code_identity.strategy_hash(research) != code_identity.strategy_hash(installed):
        raise ValueError(f"{research.name} as built for research is no longer the code installed for the paper engine: "
                         "a backtest of it is not a backtest of what traded")
    # What traded: the paper engine's core, as it reports it, and the library it loads the class from.
    traded = installed if installed.is_file() else research
    code = {"engine": client.get_status()["code"],
            "strategy": code_identity.strategy_hash(traded) if traded.is_file() else ""}
    limits = {l["name"]: l["value"] for l in client.get_risk()["limits"]}
    version_id = promotion.version_of(strategy)
    costs = (promotion.research_costs(version_id) if version_id else ledger.Costs(fee_rate=fee_rate)).model_copy(
        update={"risk": limits})
    paper = paper_fills(client, day, strategy).get(strategy, [])
    backtest, backtest_slippage = backtest_fills(data or _data() / "recorded", record["portfolio"],
                                                 record["class_name"], record["settings"], costs, day)

    by_key: dict[tuple[str, str], list[list[dict]]] = defaultdict(lambda: [[], []])
    for source, fills in ((0, paper), (1, backtest)):
        for f in fills:
            by_key[(f["symbol"], f["side"])][source].append(f)
    show = lambda f: f"{f['quantity']:g}@{f['price']:.2f} {_clock(f['time'])}" if f else "-"
    pairs, lines, unmatched, worse = [], [], 0, 0.0
    lines.append(f"{strategy} ({record['class_name']} on {record['portfolio']}), {day}: "
                 f"{len(paper)} paper fills, {len(backtest)} backtest fills\n")
    lines.append(f"{'contract':<28}{'side':<7}{'paper':>18}{'backtest':>18}{'price diff':>12}")
    for (symbol, side), (p, b) in sorted(by_key.items()):
        for i in range(max(len(p), len(b))):
            pf = p[i] if i < len(p) else None
            bf = b[i] if i < len(b) else None
            unmatched += pf is None or bf is None
            diff = pf["price"] - bf["price"] if pf and bf else None
            if diff is not None:  # what the paper fill paid beyond the backtest's
                worse += (diff if side == "BUY" else -diff) * pf["quantity"] * _multiplier(symbol)
            pairs.append({"symbol": symbol, "side": side, "paper": show(pf),
                          "backtest": show(bf), "price_diff": diff})
            lines.append(f"{symbol:<28}{side:<7}{show(pf):>18}{show(bf):>18}"
                         f"{f'{diff:+.2f}' if diff is not None else 'only one':>12}")
    lines.append(f"\ncash flow: paper {_cash(paper):+,.2f}, backtest {_cash(backtest):+,.2f} "
                 f"(a day that ends flat: its P&L, before fees)")
    lines.append(f"fills on one side only: {unmatched}")
    lines.append(f"paid over the mids: backtest {backtest_slippage:,.2f}, paper {backtest_slippage + worse:,.2f}")
    return {"status": "ok", "strategy": strategy, "class_name": record["class_name"], "setting": record["settings"],
            "code": code,
            "day": str(day), "pairs": pairs, "paper_cash": _cash(paper), "backtest_cash": _cash(backtest),
            "paper_fills": len(paper), "backtest_fills": len(backtest), "unmatched": unmatched,
            "backtest_slippage": backtest_slippage, "paper_slippage": backtest_slippage + worse,
            "text": "\n".join(lines)}


# ── report ────────────────────────────────────────────────────────────────────


def _watched(watched: dict[str, Any]) -> list[str]:
    """promotion.watch() as the report's lines."""
    rate = watched["realization_rate"]
    lines = [f"Realization rate: {rate:.0%} of {watched['judged']} versions inside their prediction" if rate is not None
             else "Realization rate: no version has enough paper days yet", ""]
    for r in watched["versions"]:
        if "error" in r:
            lines.append(f"- {r['version_id']} ({r['strategy']}, {r['stage']}): {r['error']}")
        elif r["all"] is None:
            lines.append(f"- {r['version_id']} ({r['strategy']}, {r['stage']}): too few paper days")
        else:
            a, recent = r["all"], r["recent"]
            lines.append(f"- {r['version_id']} ({r['strategy']}, {r['stage']}): {a['days']} paper days "
                         f"{a['net']:+,.0f}, predicted {a['low']:+,.0f} to {a['high']:+,.0f}; last {recent['days']} "
                         f"{recent['net']:+,.0f}, predicted from {recent['low']:+,.0f}")
    lines += [f"- {d['version_id']} back to {d['to']}: {d['reason']}" for d in watched["demoted"]]
    return lines


def recording(day: date, record_dir: Path) -> list[str]:
    """The day's recorded quotes, as the lines to report."""
    found = sorted(record_dir.glob(f"*/*-{day:%Y-%m}/{day:%Y%m%d}.parquet"))
    if not found:
        return [f"No quotes recorded for {day} under {record_dir}."]
    return [f"Quotes recorded: {', '.join(f.parent.parent.name for f in found)} ({record_dir})"]


def minute_rv(day: date, record_dir: Path) -> list[str]:
    """The day's realized vol of each recorded portfolio's minutes added to the values by day (what ComboStrategy
    reads the next day), as the lines to report."""
    lines = []
    for found in sorted(record_dir.glob(f"*/*-{day:%Y-%m}/{day:%Y%m%d}.parquet")):
        portfolio = found.parent.parent.name
        try:
            path = daily_values.add_minute_rv([found], portfolio)
            value = daily_values.read_series(path).get(str(day))
            lines.append(f"{portfolio}_minute_rv: {value:.3f}%" if value is not None
                         else f"{portfolio}_minute_rv: too few minutes recorded, none for {day}")
        except Exception as exc:
            lines.append(f"{portfolio}_minute_rv: not added: {exc}")
    return lines


def _asked(ask) -> Any:
    """What the engine answered, or {"error": its gRPC code} when it did not."""
    try:
        return ask()
    except Exception as exc:
        return {"error": _code(exc)}


def collect(client: EngineClient, day: date, record_dir: Path | None, account: str = "paper") -> dict[str, Any]:
    """The day's facts from the account's engine (the system's: for the paper account it records each strategy's paper
    day against a backtest of the recorded day, and has every version that trades watched against its prediction,
    promotion.watch): the engine's status, each strategy's holding summary and fills, the risk state, the broker
    check, the days recorded, the comparisons. A part the engine did not answer is {"error": code}."""
    facts: dict[str, Any] = {"day": day,
                             "status": _asked(client.get_status),
                             "holdings": _asked(lambda: client.get_strategy_holdings()["holdings"]),
                             "fills": _asked(lambda: paper_fills(client, day)),
                             "strategies": _asked(client.list_strategies),
                             "risk": _asked(client.get_risk),
                             "account": _asked(client.get_account),
                             "recording": None, "compared": {}}
    record_dir = record_dir or _data() / "recorded"
    if account == "paper" and record_dir.is_dir():
        facts["recording"] = recording(day, record_dir)
        facts["recording"] += minute_rv(day, record_dir)
        fills = facts["fills"] if "error" not in facts["fills"] else {}
        strategies = facts["strategies"] if isinstance(facts["strategies"], list) else []
        # Every strategy running, fills or not: a day the backtest trades and the paper engine does not is a gap too.
        for name in sorted(set(fills) | {s["strategy_name"] for s in strategies if s["status"] == "running"}):
            try:
                compared = compare(client, name, day, data=record_dir)
                evidence.record_paper_day(name, compared["class_name"], compared["setting"], str(day),
                                          compared["paper_cash"], compared["backtest_cash"], compared["paper_fills"],
                                          compared["backtest_fills"], compared["unmatched"], compared["code"],
                                          compared["backtest_slippage"], compared["paper_slippage"])
                facts["compared"][name] = compared["text"]
            except Exception as exc:
                facts["compared"][name] = f"not compared: {exc}"
    try:
        facts["watched"] = promotion.watch() if account == "paper" else {"demoted": []}
    except Exception as exc:
        facts["watched"] = {"error": str(exc), "demoted": []}
    return facts


def render(facts: dict[str, Any]) -> str:
    """collect's facts as the day's Markdown; reads nothing else."""
    day, status, holdings = facts["day"], facts["status"], facts["holdings"]
    lines = [f"# Optra {day}", "",
             f"Engine: does not answer ({status['error']})" if "error" in status else f"Engine: {status['detail']}", ""]
    fills = {} if "error" in facts["fills"] else facts["fills"]
    if "error" in facts["fills"]:
        lines += [f"No fills ({facts['fills']['error']})", ""]
    strategies = facts["strategies"] if isinstance(facts["strategies"], list) else []
    summaries = {} if "error" in holdings else {n: h.get("summary", {}) for n, h in holdings.items()}
    lines += ["## Strategies", "",
              "| strategy | status | fills today | P&L (holding) | " + " | ".join(PNL_SOURCES) + " |",
              "|---|---|---|---|" + "---|" * len(PNL_SOURCES)]
    for st in strategies:
        summary = summaries.get(st["strategy_name"], {})
        pnl = summary.get("pnl")
        lines.append(f"| {st['strategy_name']} | {st['status']} | {len(fills.get(st['strategy_name'], []))} | "
                     f"{'-' if pnl is None else f'{pnl:+,.2f}'} | "
                     + " | ".join(f"{summary.get(f'pnl_{k}', 0.0):+,.2f}" for k in PNL_SOURCES) + " |")
    lines += ["", "P&L by source: its change since the engine started today (delta, gamma, theta, vega by the "
              "Greeks held, other what they do not explain, fills against the mid).", ""]
    for name, rows in fills.items():
        lines += [f"### {name}: fills", "", "| time | contract | side | price | quantity |", "|---|---|---|---|---|"]
        lines += [f"| {_clock(f['time'])} | {f['symbol']} | {f['side']} | {f['price']:.2f} | {f['quantity']:g} |"
                  for f in rows]
        lines.append("")

    lines += ["## Risk", ""]
    risk = facts["risk"]
    if "error" in risk:
        lines.append(f"No risk state ({risk['error']})")
    else:
        lines.append(f"Halted: {risk['halt_reason']} (since {risk['halted_at']} UTC)" if risk["halted"]
                     else "Not halted.")
        lines.append(f"Orders refused since the engine started: {risk['refused']}")
        lines += [f"- {line}" for line in risk["recent_refusals"][-10:]]
        lines.append("Limits: " + ", ".join(f"{l['name']}={l['value']:g}" for l in risk["limits"]))
    lines += ["", "## Positions against IB", ""]
    account = facts["account"]
    lines += [f"Not checked ({account['error']})" if "error" in account
              else f"Check: {account['broker_check']} {account['broker_check_problem']}".strip(), ""]

    if facts["recording"] is not None:
        lines += ["## Paper against a backtest of the day", "", *facts["recording"]]
        for name, text in facts["compared"].items():
            lines += ["", f"### {name}", "", "```", text, "```"]
        lines.append("")

    lines += ["## Versions against their prediction", ""]
    watched = facts["watched"]
    lines += [f"Not watched: {watched['error']}"] if "error" in watched else _watched(watched)
    return "\n".join(lines) + "\n"


def report(client: EngineClient, day: date | None, record_dir: Path | None, send: bool,
           account: str = "paper") -> dict[str, Any]:
    """The day (New York date, default today) of the account's engine: its facts collected (collect: each strategy's
    fills and P&L, the orders refused and the halt, the broker check; for the paper account, with the recorded quotes,
    record_dir, default data/recorded, each strategy compared with a backtest of the day and the day recorded, and
    every version that trades watched against its prediction), rendered as Markdown in data/reports/<day>.md
    (<day>-<account>.md for another account; render); with send, a summary sent."""
    day = day or datetime.now(ET).date()
    facts = collect(client, day, record_dir, account)
    markdown = render(facts)
    out = _data() / "reports"
    out.mkdir(parents=True, exist_ok=True)
    path = out / (f"{day}.md" if account == "paper" else f"{day}-{account}.md")
    path.write_text(markdown)
    fills = {} if "error" in facts["fills"] else facts["fills"]
    traded = ", ".join(f"{n} {len(r)} fills" for n, r in fills.items()) or "no fills"
    summary = f"{_named(account)} {day}: {traded}." + "".join(f" {d['version_id']} back to {d['to']}."
                                                 for d in facts["watched"]["demoted"])
    summary += f" Report: {path}"
    if send:
        notify(summary)
    return {"status": "ok", "day": str(day), "path": str(path), "summary": summary, "markdown": markdown}
