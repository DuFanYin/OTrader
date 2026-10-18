"""The backtest an agent runs (the tool backtest): a strategy over an underlying's research days, one setting or a space
of them, as an experiment registered and counted in its family before it runs, waited for, and read back in one answer:
each trial's measures, the control's (RandomBuyStrategy) on the same days, what the tries (every trial in the ledger) leave of the best
(deflated Sharpe, overfitting probability, walk-forward, without its best days, by the underlying's move), and, when the
best beats the control, its robustness checks, run then too. What review asks for is listed, met or not.

And a trial run again (replay), to see on its days what it did and what its strategy logged."""

from __future__ import annotations

from datetime import date
from typing import Any

from optra.research import analysis, batch, combo, jobs, ledger, promotion, report, runner
from optra.data.days import day_of

CONTROL, CONTROL_FAMILY = ledger.CONTROL, ledger.CONTROL_FAMILY


def _measures(trial_id: str) -> dict[str, Any]:
    out = report.measures(report.trades_in_ledger(trial_id))
    months = batch.day_measures([(d["day"], d["net_pnl"]) for d in ledger.trial_days([trial_id])[trial_id]])
    return ({k: v for k, v in out.items() if k != "expectancy"} | {"by_month": months.get("by_month")}
            | {"expectancy_r": out.get("expectancy")})


def _net(experiment: dict[str, Any] | None) -> list[dict[str, Any]]:
    """Each trial of a robustness check: its setting and what it made."""
    if not experiment:
        return []
    trials = ledger.experiment(experiment["experiment_id"])["trials"]
    return [{"setting": t["setting"], "net": (t["summary"] or {}).get("net"), "trades": (t["summary"] or {}).get("trades"),
             "error": t["error"]} for t in trials]


def backtest(*, strategy: str, setting: dict[str, float] | None, space: dict[str, Any] | None,
             search: dict[str, Any] | None, family: str | None, symbol: str, start: date | None, end: date | None,
             costs: ledger.Costs | dict[str, Any], note: str = "", config: str | None = None) -> dict[str, Any]:
    cut = ledger.holdout_from(symbol)
    if end is None and cut is not None:
        end = date.fromordinal(cut.toordinal() - 1)
    days = [str(f) for f in combo.days(symbol, start, end, "backtest")]
    common = {"days": days, "costs": costs, "priority": jobs.PRIORITY_REPORT}
    exp = ledger.submit(family=family or strategy, strategy=strategy, space=space or {}, search=search or {},
                        settings_list=None if space else [setting or {}], note=note, config=config, **common)
    control = ledger.submit(family=CONTROL_FAMILY, strategy=CONTROL[0], settings_list=[CONTROL[1]],
                            note="the control of a backtest", **common)
    jobs.wait(ref=exp["experiment_id"])
    jobs.wait(ref=control["experiment_id"])

    found = analysis.analysis(exp["experiment_id"])
    if "why" in found:
        errors = ledger.trial_errors(exp["trial_ids"])
        raise ValueError(f"no trial finished: {'; '.join(sorted(set(errors.values()))) or found['why']}")
    trials = ledger.experiment(exp["experiment_id"])["trials"]
    rows = [{"id": t["id"], "setting": t["setting"], **(_measures(t["id"]) if t["status"] == "done" else {}),
             "max_margin": (t["summary"] or {}).get("max_margin"), "status": t["status"], "error": t["error"]}
            for t in trials]
    rows.sort(key=lambda r: r.get("expectancy_r") if r.get("expectancy_r") is not None else float("-inf"),
              reverse=True)
    best = next(r for r in rows if r["id"] == found["best"]["id"])
    stopped = {t["id"]: t["error"] for t in trials if t["status"] == "done" and t["error"]}
    control_row = _measures(control["trial_ids"][0])
    beats = (best.get("expectancy_r") or float("-inf")) > (control_row.get("expectancy_r") or float("-inf")) \
        and found["best"]["summary"]["net"] > 0

    robust = None
    if beats:
        checks = analysis.robustness(exp["experiment_id"], best["id"], priority=jobs.PRIORITY_REPORT)
        keys = ("costs_doubled", "fills_shown_size_late", "fills_at_the_quotes", "parameters_moved")
        for key in keys:
            if checks.get(key):
                jobs.wait(ref=checks[key]["experiment_id"])
        robust = {key: _net(checks.get(key)) for key in keys}

    dsr, pbo = found["deflated_sharpe"].get("dsr"), found["pbo"].get("pbo")

    def nets(key: str) -> list[float | None]:
        return [r["net"] for r in (robust or {}).get(key, [])]

    best_trades = ledger.trial_trades([best["id"]])[best["id"]]
    trade_days = len({t["opened"] for t in best_trades})
    review = [
        {"gate": "deflated Sharpe", "met": dsr is not None and dsr > promotion.DSR_BAR,
         "value": dsr, "bar": f"> {promotion.DSR_BAR}"},
        {"gate": "overfitting probability", "met": pbo is not None and pbo < promotion.PBO_BAR,
         "value": pbo, "bar": f"< {promotion.PBO_BAR}"},
        {"gate": "trades", "met": best.get("trades", 0) >= promotion.MIN_TRADES, "value": best.get("trades", 0),
         "bar": f">= {promotion.MIN_TRADES}"},
        {"gate": "days with a trade", "met": trade_days >= promotion.MIN_TRADE_DAYS, "value": trade_days,
         "bar": f">= {promotion.MIN_TRADE_DAYS}"},
        {"gate": promotion.RAN_ALL_DAYS, "met": not stopped, "value": len(stopped), "bar": "0 stopped"},
        {"gate": "parameters moved", "met": robust is not None and all((n or 0) > 0 for n in nets("parameters_moved")),
         "value": min(nets("parameters_moved"), key=lambda n: n or 0, default=None), "bar": "every net > 0"},
        {"gate": "costs doubled", "met": bool(nets("costs_doubled")) and all((n or 0) > 0 for n in nets("costs_doubled")),
         "value": nets("costs_doubled")[:1] or None, "bar": "net > 0"},
        {"gate": "fills of the size shown, a minute late",
         "met": bool(nets("fills_shown_size_late")) and all((n or 0) > 0 for n in nets("fills_shown_size_late")),
         "value": nets("fills_shown_size_late")[:1] or None, "bar": "net > 0"},
    ]
    if ledger.Costs.of(costs).at_the_quotes() is not None:  # it filled inside the spread
        review.append({"gate": "every leg at its bid or ask",
                       "met": bool(nets("fills_at_the_quotes")) and all((n or 0) > 0 for n in nets("fills_at_the_quotes")),
                       "value": nets("fills_at_the_quotes")[:1] or None, "bar": "net > 0"})
    return {
        "experiment_id": exp["experiment_id"], "config": config, "family": exp["family"], "tries": found["tries"],
        "family_tries": found["family_tries"],
        "days": len(days), "first_day": days[0].rsplit("/", 1)[-1], "last_day": days[-1].rsplit("/", 1)[-1],
        "trials": rows, "best": best, "control": control_row,
        "beats_control_by_r": None if best.get("expectancy_r") is None or control_row.get("expectancy_r") is None
        else round(best["expectancy_r"] - control_row["expectancy_r"], 4),
        "deflated_sharpe": dsr, "overfitting_probability": pbo,
        "walk_forward_net": found["walk_forward"].get("net"),
        "without_best_days": found["without_best_days"], "by_regime": found["by_regime"],
        "robustness": robust, "review": review, "stopped": stopped,
        "best_trades": best_trades,
    }


def replay(trial_id: str, days: list[date] | None = None, worst: int = 5) -> dict[str, Any]:
    """A finished trial run again as it ran (its strategy, setting, costs, days before the holdout, and code, which
    must not have changed), to see why it made or lost what it did: on the days asked for (or its `worst` worst), each
    fill with its time, the day's P&L by source, the round trips opened, and what the strategy and the engine logged.
    Nothing is registered: its result is in the ledger already, and the replay is checked against it."""
    t = ledger.trial(trial_id)
    if t["status"] != "done":
        raise ValueError(f"trial {trial_id} is {t['status']}: only a finished trial can be replayed")
    if ledger.code_of(t["strategy"]) != t["code"]:
        raise ValueError("the engine or the strategy changed since the trial ran: a replay would not be that trial")
    experiment_id = ledger.experiments_of(trial_id)[0]
    spec = ledger.experiment(experiment_id)["spec"]
    files = ledger.experiment_days(experiment_id)
    root = files[0].parent.parent.name
    cut = ledger.holdout_from(root)
    files = [f for f in files if cut is None or (day_of(f) or cut) < cut]
    if not files:
        raise ValueError(f"the trial ran only on {root}'s holdout: it is not replayed")
    stored = {d["day"]: d for d in ledger.trial_days([trial_id])[trial_id]}
    if days:
        wanted = {d.isoformat() for d in days}
    else:
        wanted = {d["day"] for d in sorted((d for d in stored.values() if not cut or d["day"] < cut.isoformat()),
                                           key=lambda d: d["net_pnl"])[:worst]}
    # Up to the last day looked at (the holding carries from the first), its log kept on those days only.
    last = max(wanted, default=None)
    files = [f for f in files if last is None or (day_of(f) or date.min).isoformat() <= last]
    run = {"files": [str(f) for f in files], "risk_free_rate": spec["pricing"]["risk_free_rate"],
           "iv_price_mode": spec["pricing"]["iv_price_mode"],
           "trials": [{"id": trial_id, "strategy": t["strategy"], "setting": t["setting"],
                       "costs": ledger.Costs.of(t["costs"]).model_dump(), "fills": True, "log": True,
                       "log_days": sorted(wanted)}]}
    ran, trials = jobs.run(run, jobs.PRIORITY_REPORT)
    out = trials[0]
    trips = {}
    for trade in ledger.trial_trades([trial_id])[trial_id]:
        trips.setdefault(trade["opened"], []).append(trade)
    underlying = dict(zip(ran["date"], zip(ran["underlying_open"], ran["underlying_close"])))
    fills: dict[int, list[dict[str, Any]]] = {}
    for f in runner.rows(out["fills"]):
        fills.setdefault(f["date"], []).append({"time": f["time"], "symbol": f["symbol"], "side": f["side"],
                                                 "price": f["price"], "quantity": f["quantity"],
                                                 "order": f["order_id"]})
    log: dict[int, list[dict[str, Any]]] = {}
    for r in runner.rows(out["log"]):
        log.setdefault(r["date"], []).append({k: r[k] for k in ("time", "level", "source", "msg")})
    shown, differs = [], []
    for d in runner.rows(out["days"]):
        if not d["date"]:
            continue
        day = ledger.iso_day(d["date"])
        if day in stored and abs(stored[day]["net_pnl"] - d["net_pnl"]) > 0.01:
            differs.append(day)
        if day not in wanted:
            continue
        shown.append({"day": day, "net": round(d["net_pnl"], 2), "fees": round(d["fees"], 2), "flat": d["flat"],
                      "attribution": {k: d[k] for k in batch.ATTRIBUTION},
                      "underlying": list(underlying.get(d["date"], (None, None))),
                      "round_trips_opened": trips.get(day, []), "fills": fills.get(d["date"], []),
                      "log": log.get(d["date"], [])})
    return {"trial_id": trial_id, "strategy": t["strategy"], "setting": t["setting"], "costs": t["costs"],
            "days_run": len(files), "days": shown, "errors": out.get("errors") or [],
            "differs_from_ledger": differs}
