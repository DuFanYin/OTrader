"""Strategies over backtest days, trade by trade, by the promotion measures (POST /api/backtest/report).

The strategies are trials in the ledger (one experiment per class, run at once in one batch each, a trial run
before reused) and their round trips are read back from it; the days run in date order with the holding carried
overnight.
A strategy that opens one position a day and closes it the same day (the 0DTE buyers, ComboStrategy) makes one trade
a day. Reported, apart for the research days and the holdout (holdout_from), and for all: trades, win rate, average
win and loss in R, payoff (average win / average loss, in USD), expectancy (mean R) with its t (mean / sd x
sqrt(n)), net USD, the longest losing streak, the deepest drawdown (USD, cumulative), and the gap to the control
(RandomBuyStrategy) when it is run. Days in the holdout are the system's and the person's (infra/access.py): the
agent's report stops before them, and reaches them only through a version's holdout report
(promotion.holdout_report); each use is recorded in the ledger.

A strategy needs at least 300 independent trades before it is promoted; fewer, and the numbers say what to look
at, not what works.
"""

from __future__ import annotations

import statistics
from datetime import date
from pathlib import Path
from typing import Any

from optra.infra import access
from optra.research import jobs, ledger
from optra.research.ledger import Costs
from optra.research import batch
from optra.research.batch import ATTRIBUTION, Trade, days_of
from optra.utils.files import inside_data

COLUMNS = ["trades", "win rate", "avg win R", "avg loss R", "payoff", "expectancy R", "t",
           "net USD", "losing streak", "drawdown USD"]


def per_trade(trades: list[Trade]) -> list[dict[str, Any]]:
    """Each trade: the day it opened and the days it was open, net after fees, risk, R, share fills, P&L by source,
    fees, and what its orders paid over the mids and its options settled for."""
    return [{"day": t.day.isoformat(), "days": t.days, "net": round(t.net, 2), "risk": round(t.risk, 2),
             "r": round(t.r, 4), "hedge_fills": t.hedge_fills,
             "attribution": {k: round(v, 2) for k, v in t.attribution.items()}, "fees": round(t.fees, 2),
             "entry_cost": round(t.entry_cost, 2), "exit_cost": round(t.exit_cost, 2), "settled": round(t.settled, 2)}
            for t in trades]


def measures(trades: list[Trade]) -> dict[str, Any]:
    n = len(trades)
    if n == 0:
        return {"trades": 0}
    wins = [t for t in trades if t.net > 0]
    losses = [t for t in trades if t.net <= 0]
    mean, t_r = batch.r_stats([t.r for t in trades])
    streak = longest = 0
    for t in trades:
        streak = streak + 1 if t.net <= 0 else 0
        longest = max(longest, streak)
    drawdown = batch.drawdown(t.net for t in trades)
    avg_win = statistics.fmean(t.net for t in wins) if wins else 0.0
    avg_loss = statistics.fmean(t.net for t in losses) if losses else 0.0
    return {
        "trades": n,
        "win rate": f"{len(wins) / n:.0%}",
        "avg win R": f"{statistics.fmean(t.r for t in wins):+.2f}" if wins else "-",
        "avg loss R": f"{statistics.fmean(t.r for t in losses):+.2f}" if losses else "-",
        "payoff": f"{avg_win / abs(avg_loss):.2f}" if avg_loss else "-",
        "expectancy R": f"{mean:+.3f}",
        "t": f"{t_r:+.2f}" if t_r is not None else "-",
        "net USD": f"{sum(t.net for t in trades):+,.0f}",
        "losing streak": longest,
        "drawdown USD": f"{drawdown:,.0f}",
        "expectancy": mean,
        "attribution": {k: round(sum(t.attribution[k] for t in trades), 2) for k in ATTRIBUTION},
        "fees": round(sum(t.fees for t in trades), 2),
        "entry_cost": round(sum(t.entry_cost for t in trades), 2),
        "exit_cost": round(sum(t.exit_cost for t in trades), 2),
        "settled": round(sum(t.settled for t in trades), 2),
    }


def table(rows: list[dict[str, Any]], control: float | None) -> str:
    head = ["strategy", *COLUMNS] + (["vs control R"] if control is not None else [])
    lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    for row in rows:
        cells = [row["strategy"], *[str(row.get(c, "-")) for c in COLUMNS]]
        if control is not None:
            cells.append(f"{row['expectancy'] - control:+.3f}" if "expectancy" in row else "-")
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


def trades_in_ledger(trial_id: str) -> list[Trade]:
    """A finished trial's round trips, as the ledger keeps them."""
    return [Trade(date.fromisoformat(t["opened"]), t["risk"], t["net"], {k: t[k] for k in ATTRIBUTION},
                  t["hedge_fills"], t["days"], t["fees"], t["entry_cost"], t["exit_cost"], t["settled"])
            for t in ledger.trial_trades([trial_id])[trial_id]]


def report(days: Path | list[Path], runs: dict[str, tuple[str, dict[str, float]]], holdout_from: date | None,
           costs: Costs, family: str | None = None, pricing: dict[str, Any] | None = None) -> dict[str, Any]:
    """days: a folder of backtest days, or one (relative: to the repository), or the days themselves. costs: the
    trials' (ledger.Costs); pricing: the risk-free rate and the IV's price (ledger.submit's defaults when none). The
    strategies are trials in the ledger, counted in `family`
    (default: each strategy's class): one experiment per class, all submitted at the report's priority and then
    waited for (the same days: one batch), their trials' round trips read back from the ledger; a trial run before is
    not run again."""
    if isinstance(days, list):
        files = [inside_data(d) for d in days]
    else:
        days = inside_data(days)
        files = days_of(days)
    if not files:
        raise ValueError(f"no backtest days under {days}")
    strategies = list(runs)  # each run's name: the rows' and the trades'
    by_class: dict[str, list[str]] = {}
    for spec, (cls, _) in runs.items():
        by_class.setdefault(cls, []).append(spec)
    trial_of: dict[str, str] = {}
    experiments = []
    submitted = ledger.submit_together([  # together: on the same days, they run as one batch
        dict(family=ledger.CONTROL_FAMILY if cls == ledger.CONTROL[0] else family or cls, strategy=cls,
             days=[str(f) for f in files], costs=costs, settings_list=[runs[spec][1] for spec in specs],
             reach_holdout=access.trusted(), report=True, note="promotion report", priority=jobs.PRIORITY_REPORT,
             **(pricing or {}))
        for cls, specs in by_class.items()])
    for specs, out in zip(by_class.values(), submitted):
        experiments.append(out["experiment_id"])
        trial_of.update(zip(specs, out["trial_ids"]))
    for experiment_id in experiments:
        jobs.wait(ref=experiment_id)
    results = {spec: trades_in_ledger(trial_of[spec]) for spec in strategies}
    failed = ledger.trial_errors(trial_of.values())
    errors = {spec: failed[trial_of[spec]].split("; ") for spec in strategies if trial_of[spec] in failed}

    def period(trades: list[Trade], which: str) -> list[Trade]:
        if holdout_from is None or which == "all":
            return trades
        return [t for t in trades if (t.day >= holdout_from) == (which == "holdout")]
    header = (f"{len(files)} days, {files[0].name} to {files[-1].name}; costs "
              + ", ".join(f"{k} {v}" for k, v in costs.model_dump().items() if v != {})
              + "; R = a day's P&L after fees / what the position opened could lose at most")
    periods = []
    for which in ["all"] if holdout_from is None else ["research", "holdout", "all"]:
        rows = [{"strategy": spec, **measures(period(trades, which))} for spec, trades in results.items()]
        control = next((r["expectancy"] for r in rows
                        if r["strategy"].split(":")[0] == "RandomBuyStrategy" and "expectancy" in r), None)
        label = {"research": f"research (before {holdout_from})",
                 "holdout": f"holdout (from {holdout_from})", "all": "all days"}[which]
        periods.append({"period": which, "label": label, "control": control, "rows": rows,
                        "markdown": f"### {label}\n\n{table(rows, control)}"})
    markdown = header + "\n\n" + "\n\n".join(p["markdown"] for p in periods) + "\n"
    return {"status": "ok", "days": len(files), "header": header, "periods": periods,
            "trades": {spec: per_trade(trades) for spec, trades in results.items()},
            "errors": errors, "markdown": markdown}
