"""The live engine's trading day: is it well (check), what happened (report), and did paper trade as
a backtest of the same day would have (compare).

A scheduler drives it through the API (POST /api/session/check, POST /api/session/report; the
systemd timers in ~/repo/dufanyin/deploy). With notify, a message goes out through
$OPTRA_NOTIFY (default `notify`, its one argument the text): for check only when what is wrong
changes, for report a summary.
"""

from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
from collections import defaultdict
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

from optra.services import recorded_days
from optra.infra.remote_client import EngineClient
from optra.utils.files import repo_root

NEW_YORK = ZoneInfo("America/New_York")
OPTION = re.compile(r"-\d{8}-[CP]-")


PNL_SOURCES = ("delta", "gamma", "theta", "vega", "other", "fills")  # the holding summary's pnl_*

def notify(text: str) -> None:
    command = shlex.split(os.environ.get("OPTRA_NOTIFY", "notify"))
    try:
        subprocess.run([*command, text], timeout=60, check=False)
    except OSError as exc:
        print(f"notify: {exc}")


def _data() -> Path:
    return repo_root() / "data"


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
    except Exception as exc:
        out.append(f"no risk state ({_code(exc)})")
    try:
        account = client.get_account()
        if account["positions_check"] not in ("agree", ""):
            out.append(f"positions check: {account['positions_check']} "
                       f"{account['positions_problem']}".strip())
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


def check(client: EngineClient, send: bool) -> dict[str, Any]:
    """problems(); with send, a message when the notified problems change (state kept in
    data/session-check.json). A problem is notified once it is seen by two checks in a row: the
    engine does not answer for about a minute while it starts (the morning restart, a deploy), and
    a crash is notified separately (optra-alert@). Recovery is notified for the problems that
    were notified, naming them."""
    now = problems(client)
    state = _data() / "session-check.json"
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
        message = "Optra: " + "; ".join(persistent)
    elif not persistent and notified and not now:
        message = "Optra: back to normal; was: " + "; ".join(notified)
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
    records = client.get_orders_and_trades(strategy, 0)["records"]
    out: dict[str, list[dict]] = {}
    for r in sorted((r for r in records if r["record_type"] == "Trade"), key=lambda r: r["timestamp"]):
        at = _when(r)
        if at.astimezone(NEW_YORK).date() == day:
            out.setdefault(r["strategy_name"], []).append(
                {"symbol": r["symbol"], "direction": r["direction"], "price": r["price"],
                 "volume": r["volume"], "time": at})
    return out


# ── compare: a paper day against a backtest of it ────────────────────────────


def _multiplier(symbol: str) -> float:
    return 100.0 if OPTION.search(symbol) else 1.0


def _cash(fills: list[dict]) -> float:
    """What the fills paid (negative) and received: a day flat at the end, its P&L."""
    return sum((1 if f["direction"] == "SHORT" else -1) * f["price"] * f["volume"]
               * _multiplier(f["symbol"]) for f in fills)


def _clock(t: datetime | None) -> str:
    return t.astimezone(NEW_YORK).strftime("%H:%M:%S") if t else "-"


def backtest_fills(data: Path, portfolio: str, cls: str, setting: dict[str, float],
                   risk: dict[str, float], fee_rate: float, day: date) -> list[dict]:
    """The strategy's fills in a backtest of the recorded day (engine/build/entry_backtest)."""
    file = data / portfolio / f"{portfolio}-{day:%Y-%m}" / f"{day:%Y%m%d}.parquet"
    if not file.exists():
        raise ValueError(f"no recorded day {file} (POST /api/data/recorded)")
    entry = repo_root() / "engine" / "build" / "entry_backtest"
    params = [f"{k}={v:g}" for k, v in setting.items()] + [f"risk.{k}={v:g}" for k, v in risk.items()]
    run = subprocess.run([str(entry), "--files", str(file), cls, "--fee-rate", f"{fee_rate:g}",
                          *params], capture_output=True, text=True, timeout=600)
    if run.returncode != 0:
        raise ValueError(f"entry_backtest failed ({run.returncode}): {run.stderr.strip()[-500:]}")
    result = json.loads(run.stdout)
    if result.get("errors"):
        raise ValueError(f"entry_backtest: {result['errors']}")
    fills = []
    for daily in result.get("daily_results", []):
        for t in daily.get("trades", []):
            at = datetime.strptime(t["time"], "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc) \
                if t["time"] else None
            fills.append({"symbol": t["symbol"], "direction": t["direction"], "price": t["price"],
                          "volume": t["volume"], "time": at})
    return fills


def compare(client: EngineClient, strategy: str, day: date, fee_rate: float = 0.65,
            data: Path | None = None) -> dict[str, Any]:
    """A paper day against a backtest of the same recorded day: the strategy's class, its setting
    and the risk limits as the live engine has them, over the day's recorded quotes (data/
    recorded-parquet). Each contract and side in time order, the n-th paper fill against the n-th
    backtest one; what only one side has; each side's cash flow.

    Differences to expect: IB's paper fills are not the backtest's (it fills at the bid or ask of
    the minute's quotes, plus slippage); recorded quotes are the minute's last, and delayed ones
    when the paper account has no real-time data."""
    record = next((s for s in client.list_strategies() if s["strategy_name"] == strategy), None)
    if record is None:
        raise ValueError(f"no strategy {strategy}")
    risk = {l["name"]: l["value"] for l in client.get_risk()["limits"]}
    paper = paper_fills(client, day, strategy).get(strategy, [])
    backtest = backtest_fills(data or _data() / "recorded-parquet", record["portfolio"],
                              record["class_name"], record["settings"], risk, fee_rate, day)

    by_key: dict[tuple[str, str], list[list[dict]]] = defaultdict(lambda: [[], []])
    for side, fills in ((0, paper), (1, backtest)):
        for f in fills:
            by_key[(f["symbol"], f["direction"])][side].append(f)
    show = lambda f: f"{f['volume']:g}@{f['price']:.2f} {_clock(f['time'])}" if f else "-"
    pairs, lines, unmatched = [], [], 0
    lines.append(f"{strategy} ({record['class_name']} on {record['portfolio']}), {day}: "
                 f"{len(paper)} paper fills, {len(backtest)} backtest fills\n")
    lines.append(f"{'contract':<28}{'side':<7}{'paper':>18}{'backtest':>18}{'price diff':>12}")
    for (symbol, direction), (p, b) in sorted(by_key.items()):
        for i in range(max(len(p), len(b))):
            pf = p[i] if i < len(p) else None
            bf = b[i] if i < len(b) else None
            unmatched += pf is None or bf is None
            diff = pf["price"] - bf["price"] if pf and bf else None
            pairs.append({"symbol": symbol, "direction": direction, "paper": show(pf),
                          "backtest": show(bf), "price_diff": diff})
            lines.append(f"{symbol:<28}{direction:<7}{show(pf):>18}{show(bf):>18}"
                         f"{f'{diff:+.2f}' if diff is not None else 'only one':>12}")
    lines.append(f"\ncash flow: paper {_cash(paper):+,.2f}, backtest {_cash(backtest):+,.2f} "
                 f"(a day that ends flat: its P&L, before fees)")
    lines.append(f"fills on one side only: {unmatched}")
    return {"status": "ok", "strategy": strategy, "day": str(day), "pairs": pairs,
            "paper_cash": _cash(paper), "backtest_cash": _cash(backtest), "unmatched": unmatched,
            "text": "\n".join(lines)}


# ── report ────────────────────────────────────────────────────────────────────


def convert_recording(day: date, record_dir: Path, data_out: Path) -> list[str]:
    """The day's recorded quotes as backtest days; the lines to report."""
    found = sorted(record_dir.glob(f"*/*-{day:%Y-%m}/{day:%Y%m%d}.csv"))
    if not found:
        return [f"No quotes recorded for {day} under {record_dir}."]
    written, failed = recorded_days.convert(found, data_out, overwrite=True)
    lines = [f"Recorded quotes converted: {len(written)} day file(s) -> {data_out}"]
    lines += [f"Conversion failed: {line}" for line in failed]
    return lines


def report(client: EngineClient, day: date | None, record_dir: Path | None, send: bool) -> dict[str, Any]:
    """The day (New York date, default today) as Markdown in data/reports/<day>.md: each
    strategy's fills and P&L, the orders refused and the halt, the positions check; with the
    recorded quotes (record_dir, default data/recorded), the day converted to a backtest day and
    each strategy that traded compared with a backtest of it."""
    day = day or datetime.now(NEW_YORK).date()
    lines = [f"# Optra {day}", ""]
    try:
        lines += [f"Engine: {client.get_status()['detail']}", ""]
    except Exception as exc:
        lines += [f"Engine: does not answer ({_code(exc)})", ""]

    pnl_of: dict[str, float] = {}
    sources_of: dict[str, dict[str, float]] = {}
    try:
        for name, h in client.get_strategy_holdings()["holdings"].items():
            summary = h.get("summary", {})
            pnl_of[name] = summary.get("pnl")
            sources_of[name] = {k: summary.get(f"pnl_{k}", 0.0) for k in PNL_SOURCES}
    except Exception:
        pass
    try:
        fills = paper_fills(client, day)
    except Exception as exc:
        fills = {}
        lines += [f"No fills ({_code(exc)})", ""]
    try:
        strategies = client.list_strategies()
    except Exception:
        strategies = []
    lines += ["## Strategies", "",
              "| strategy | status | fills today | P&L (holding) | " + " | ".join(PNL_SOURCES) + " |",
              "|---|---|---|---|" + "---|" * len(PNL_SOURCES)]
    for st in strategies:
        pnl = pnl_of.get(st["strategy_name"])
        sources = sources_of.get(st["strategy_name"], {})
        lines.append(f"| {st['strategy_name']} | {st['status']} | {len(fills.get(st['strategy_name'], []))} | "
                     f"{'-' if pnl is None else f'{pnl:+,.2f}'} | "
                     + " | ".join(f"{sources.get(k, 0.0):+,.2f}" for k in PNL_SOURCES) + " |")
    lines += ["", "P&L by source: its change since the engine started today (delta, gamma, theta, vega by the "
              "Greeks held, other what they do not explain, fills against the mid).", ""]
    for name, rows in fills.items():
        lines += [f"### {name}: fills", "", "| time | contract | side | price | volume |", "|---|---|---|---|---|"]
        lines += [f"| {_clock(f['time'])} | {f['symbol']} | {f['direction']} | {f['price']:.2f} | {f['volume']:g} |"
                  for f in rows]
        lines.append("")

    lines += ["## Risk", ""]
    try:
        risk = client.get_risk()
        lines.append(f"Halted: {risk['halt_reason']} (since {risk['halted_at']} UTC)" if risk["halted"]
                     else "Not halted.")
        lines.append(f"Orders refused since the engine started: {risk['refused']}")
        lines += [f"- {line}" for line in risk["recent_refusals"][-10:]]
        lines.append("Limits: " + ", ".join(f"{l['name']}={l['value']:g}" for l in risk["limits"]))
    except Exception as exc:
        lines.append(f"No risk state ({_code(exc)})")
    lines.append("")

    lines += ["## Positions against IB", ""]
    try:
        account = client.get_account()
        lines.append(f"Check: {account['positions_check']} {account['positions_problem']}".strip())
    except Exception as exc:
        lines.append(f"Not checked ({_code(exc)})")
    lines.append("")

    record_dir = record_dir or _data() / "recorded"
    if record_dir.is_dir():
        data_out = _data() / "recorded-parquet"
        lines += ["## Paper against a backtest of the day", ""]
        lines += convert_recording(day, record_dir, data_out)
        for name in fills:
            try:
                text = compare(client, name, day, data=data_out)["text"]
            except Exception as exc:
                text = f"not compared: {exc}"
            lines += ["", f"### {name}", "", "```", text, "```"]
        lines.append("")

    out = _data() / "reports"
    out.mkdir(parents=True, exist_ok=True)
    path = out / f"{day}.md"
    markdown = "\n".join(lines) + "\n"
    path.write_text(markdown)
    traded = ", ".join(f"{n} {len(r)} fills" for n, r in fills.items()) or "no fills"
    summary = f"Optra {day}: {traded}. Report: {path}"
    if send:
        notify(summary)
    return {"status": "ok", "day": str(day), "path": str(path), "summary": summary, "markdown": markdown}
