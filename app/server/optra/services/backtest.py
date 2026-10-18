"""The page's backtest (POST /api/run_backtest): one strategy over a symbol's days, with the P&L and Greeks minute by
minute for the chart; a job at the page's priority (research.jobs) like every backtest the server runs. One runs at a
time: a new one, or POST /api/backtest/cancel, stops the one before."""

from __future__ import annotations

from datetime import date
from typing import Any

from optra.infra import access
from optra.research import batch as batches
from optra.research import combo, jobs, ledger
from optra.research.ledger import Costs

RUN = "page"  # the page's run, by name (jobs.cancel)


def run_backtest(request: dict[str, Any]) -> dict[str, Any]:
    symbol = str(request.get("parquet", "")).strip().split("/")[-1]
    strategy = request.get("strategy", "")
    if not symbol or not strategy:
        return {"status": "error", "error": "Missing required fields: parquet (the symbol), strategy"}
    if not request.get("start_date") or not request.get("end_date"):
        return {"status": "error", "error": "Missing required fields: start_date, end_date"}
    iv_price_mode = str(request.get("iv_price_mode") or "mid").lower()
    if iv_price_mode not in {"mid", "bid", "ask"}:
        return {"status": "error", "error": "iv_price_mode must be mid, bid, or ask"}
    try:
        costs = Costs.model_validate(request.get("costs") or {})
        risk_free_rate = float(request.get("risk_free_rate", 0.05) or 0.0)
    except (TypeError, ValueError) as e:
        return {"status": "error", "error": f"the costs and risk_free_rate: {e}"}
    setting = {k: float(v) for k, v in (request.get("strategy_setting") or {}).items()
               if isinstance(v, (int, float))}
    try:
        days = combo.days(symbol, date.fromisoformat(request["start_date"]), date.fromisoformat(request["end_date"]))
        if not access.trusted():  # the agent's runs stop before the holdout
            ledger.resolve_days([str(d) for d in days])
        trial = {"id": RUN, "strategy": strategy, "setting": setting, "costs": costs.model_dump(),
                 "minute_metrics": True}
        batch = {"files": [str(d) for d in days], "risk_free_rate": risk_free_rate, "iv_price_mode": iv_price_mode,
                 "trials": [trial]}
        files, trials = jobs.run(batch, jobs.PRIORITY_PAGE, name=RUN)
    except jobs.Cancelled:
        return {"status": "cancelled", "error": "Backtest cancelled"}
    except ValueError as exc:
        return {"status": "error", "error": str(exc)}
    trial = trials[0]
    trial.pop("id", None)
    # The trial's days (a column a field) summed as the ledger sums a trial's, and the days it ran: their bars
    # and quotes.
    summary = ledger.summary_of(trial["days"], batches.trades_of(trial)[0])
    return {"status": "ok", **trial, "summary": summary, "day_files": files}


def cancel_backtest() -> dict[str, Any]:
    stopped = jobs.cancel(name=RUN) > 0
    return {"status": "ok", "message": "backtest cancelled" if stopped else "no active backtest"}
