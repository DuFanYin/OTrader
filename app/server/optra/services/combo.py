"""A combo picked in the lab, run as the engine's ComboStrategy: its legs as the strategy's
rules, backtested day by day (research.run), and the hedge study.

A leg is a rule, so the same combo runs on any day: call or put, contracts per combo unit (bought
+, sold -), its strike as a percent from its expiry's forward, its expiry the first at least dte
days out. A leg from the lab's screener or Multi-Leg names a contract instead (strike, expiry, and
the forward it was priced on, on the day it was picked: as_of); it is turned into the rule that
picks that contract that day.

The hedge study runs the same combo over the same days unhedged and with the HedgeEngine keeping
its delta within each band, and gives for each the daily P&L's mean, std, quantiles, VaR and
expected shortfall at 95% (the mean of the worst 5% of days), the share fills and the fees: the
measures of the lab's hedging tool, on the days that happened instead of simulated paths.
"""

from __future__ import annotations

import math
import statistics
from datetime import date
from pathlib import Path
from typing import Any

from optra.services import chains, research

CLASS = "ComboStrategy"
MAX_LEGS = 4


def leg_rule(leg: dict[str, Any], as_of: date | None) -> dict[str, float]:
    """One leg as ComboStrategy's rule: right (0 call, 1 put), ratio, strike_pct, dte."""
    right = 0 if str(leg["option_type"]).lower().startswith("c") else 1
    ratio = int(leg.get("ratio", leg.get("qty", 0)))
    if ratio == 0:
        raise ValueError("a leg needs a non-zero ratio (bought +, sold -)")
    if leg.get("strike_pct") is not None:
        strike_pct = float(leg["strike_pct"])
        dte = int(leg.get("dte", 0))
    else:
        if as_of is None or leg.get("strike") is None or leg.get("forward") is None or not leg.get("expiry"):
            raise ValueError("a leg gives strike_pct and dte, or strike, expiry and forward with as_of")
        strike_pct = (float(leg["strike"]) / float(leg["forward"]) - 1.0) * 100.0
        dte = (date.fromisoformat(str(leg["expiry"])[:10]) - as_of).days
    return {"right": right, "ratio": ratio, "strike_pct": round(strike_pct, 4), "dte": max(dte, 0)}


def setting(legs: list[dict[str, Any]], as_of: date | None, **options: float) -> dict[str, float]:
    """ComboStrategy's setting: its legs' rules, then the other parameters given (entry_minute,
    exit_minute, quantity, take_profit, stop_loss, hedge, hedge_timer, delta_range, model,
    min_edge)."""
    if not 1 <= len(legs) <= MAX_LEGS:
        raise ValueError(f"a combo has 1 to {MAX_LEGS} legs")
    out: dict[str, float] = {}
    for i in range(MAX_LEGS):
        rule = leg_rule(legs[i], as_of) if i < len(legs) else {"right": 0, "ratio": 0, "strike_pct": 0, "dte": 0}
        for key, value in rule.items():
            out[f"leg{i + 1}_{key}"] = value
    out.update({k: v for k, v in options.items() if v is not None})
    return out


def spec(values: dict[str, float]) -> str:
    """research.run's "Class:name=value,..."."""
    return CLASS + ":" + ",".join(f"{k}={v:g}" for k, v in values.items())


def days(symbol: str, start: date | None, end: date | None, source: str = "both") -> list[Path]:
    """The symbol's backtest and recorded days from start to end (a recorded day wins over a
    backtest one), in date order."""
    found = chains._day_files(symbol)  # ISO day -> (file, "backtest" or "recorded")
    picked = [(day, f) for day, (f, kind) in sorted(found.items())
              if (source == "both" or kind == source)
              and (start is None or day >= start.isoformat()) and (end is None or day <= end.isoformat())]
    if not picked:
        raise ValueError(f"no {symbol} days{f' from {start}' if start else ''}{f' to {end}' if end else ''}")
    return [f for _, f in picked]


def backtest(symbol: str, values: dict[str, float], files: list[Path], holdout_from: date | None,
             fee_rate: float, risk: list[str], slippage_bps: float | None = None) -> dict[str, Any]:
    """The combo's report over the days (research.report: its measures and each day), and the
    strategy as POST /api/strategies takes it, to run the same on the paper account."""
    out = research.report(files, [spec(values)], holdout_from, fee_rate, risk, slippage_bps)
    out["strategy"] = {"strategy_class": CLASS, "portfolio_name": symbol, "setting": values}
    return out


def quantile(sorted_values: list[float], q: float) -> float:
    if not sorted_values:
        return math.nan
    i = q * (len(sorted_values) - 1)
    lo, hi = math.floor(i), math.ceil(i)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (i - lo)


def distribution(trades: list[research.Trade]) -> dict[str, Any]:
    """The daily P&L after fees: the lab's hedging measures."""
    nets = sorted(t.net for t in trades)
    n = len(nets)
    if n == 0:
        return {"days": 0}
    tail = nets[:max(1, math.ceil(0.05 * n))]
    return {
        "days": n,
        "mean": statistics.fmean(nets),
        "std": statistics.stdev(nets) if n > 1 else 0.0,
        "q05": quantile(nets, 0.05), "q50": quantile(nets, 0.5), "q95": quantile(nets, 0.95),
        "var95": -quantile(nets, 0.05),
        "es95": -statistics.fmean(tail),
        "hedge_fills": sum(t.hedge_fills for t in trades),
        "fees": round(sum(sum(t.attribution.values()) - t.net for t in trades), 2),
        "attribution": {k: round(sum(t.attribution[k] for t in trades), 2) for k in research.ATTRIBUTION},
    }


def hedge_study(values: dict[str, float], files: list[Path], delta_ranges: list[int], hedge_timer: int,
                fee_rate: float, risk: list[str], slippage_bps: float | None = None) -> dict[str, Any]:
    """The combo unhedged and hedged within each band, on the same days."""
    rows, errors = [], {}
    variants = [("unhedged", {"hedge": 0})] + [
        (f"within {r} shares", {"hedge": 1, "delta_range": r, "hedge_timer": hedge_timer}) for r in delta_ranges]
    for label, hedge in variants:
        trades, errs = research.run(files, spec({**values, **hedge}), fee_rate, risk, slippage_bps)
        if errs:
            errors[label] = errs
        row = {"variant": label, **hedge, **distribution(trades)}
        if hedge["hedge"] and row.get("days") and row["hedge_fills"] == 0:
            errors.setdefault(label, []).append(
                "no share was traded: the days have no quotes of what it hedges with (SPXW hedges with SPY, "
                "which its backtest days do not carry), so this is the unhedged result again")
        rows.append(row)
    priced = [r for r in rows if r.get("days")]
    best = min(priced, key=lambda r: r["es95"])["variant"] if priced else None
    return {"rows": rows, "least_tail_loss": best, "errors": errors}
