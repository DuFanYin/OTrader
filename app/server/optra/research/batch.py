"""A backtest's round trips as the engine cut them (trades_of) and as the ledger keeps them (trade_columns).

A trade is a round trip as the engine cuts it (backtest::RoundTrip): from the bar a strategy's position opens
until the bar it holds nothing again, the same day or days later. Its result is its P&L after fees, its risk what the
position opened can lose at most (StrategyHolding::max_loss), and R = result / risk; with it its fees, its P&L by
source (delta, gamma, theta, vega, other, fills), what its opening and its other orders paid over the mids, and what its
options settled for.
"""

from __future__ import annotations

import math
import statistics
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import Any

from optra.data.days import DAY_FILE

ATTRIBUTION = ("delta", "gamma", "theta", "vega", "other", "fills")


@dataclass
class Trade:
    day: date    # the day it opened
    risk: float  # USD the position opened can lose at most (see the module's doc)
    net: float   # its P&L after fees, over its days
    attribution: dict[str, float]  # its P&L by source, before fees
    hedge_fills: int = 0  # fills in shares
    days: int = 1  # the trading days it spans: 1 for one opened and closed the same day
    fees: float = 0.0
    entry_cost: float = 0.0  # USD its opening orders paid over the mids
    exit_cost: float = 0.0   # USD its other option orders paid over the mids
    settled: float = 0.0     # USD its options settled for in cash

    @property
    def r(self) -> float:
        return self.net / self.risk


def days_of(path: Path) -> list[Path]:
    """A folder of backtest days (searched below) or one day, in date order."""
    files = sorted(path.rglob("*.parquet")) if path.is_dir() else [path]
    return sorted(files, key=lambda p: DAY_FILE.search(p.name).group(1) if DAY_FILE.search(p.name) else p.name)


def label(strategy: str, setting: dict[str, float]) -> str:
    """How a class and setting are named in a report and in the holdout's record: "Class:name=value,...", the names
    sorted. Only shown and kept, never read back."""
    return strategy + ":" + ",".join(f"{k}={float(x)!r}" for k, x in sorted(setting.items()))


def parse_spec(spec: str) -> tuple[str, dict[str, float]]:
    """"Class:name=value,name=value" -> (class, setting): how a person writes a strategy in a report's input."""
    cls, _, params = spec.partition(":")
    setting = {}
    for p in (p for p in params.split(",") if p):
        name, _, value = p.partition("=")
        setting[name] = float(value)
    return cls, setting


def day_measures(days: list[tuple[str, float]]) -> dict[str, Any]:
    """A run's days (day YYYY-MM-DD, net P&L after fees): its worst day, the mean of its worst 5% of days (ES95, at
    least one day) and its net by month."""
    if not days:
        return {}
    nets = sorted(net for _, net in days)
    tail = nets[:max(1, math.ceil(0.05 * len(nets)))]
    months: dict[str, float] = {}
    for day, net in days:
        months[day[:7]] = months.get(day[:7], 0.0) + net
    return {"worst_day": round(nets[0], 2), "es95_day": round(statistics.fmean(tail), 2),
            "by_month": {m: round(v, 2) for m, v in sorted(months.items())}}


def trades_of(result: dict[str, Any]) -> tuple[list[Trade], list[str], list[str]]:
    """A backtest's round trips as the engine cut them (its "round_trips", a column a field), its errors (what stopped
    it or its strategy), and notes of the trades left out (a position opened with no bounded loss has no R, one still
    open at the end has no result): those are not errors, the run went through all its days."""
    trips = result.get("round_trips") or {}
    by_source = trips.get("attribution") or {}
    trades = []
    unbounded = 0
    still_open = None
    for i, opened in enumerate(trips.get("opened") or []):
        if trips["closed"][i] is None:
            still_open = opened[:10]
        elif trips["max_loss"][i] is None:
            unbounded += 1
        else:
            trades.append(Trade(date.fromisoformat(opened[:10]), trips["max_loss"][i], trips["net"][i],
                                {k: float(by_source[k][i]) for k in ATTRIBUTION}, trips["share_fills"][i],
                                trips["days"][i], trips["fees"][i], trips["entry_cost"][i], trips["exit_cost"][i],
                                trips["settled"][i]))
    errors = result.get("errors", [])[:3]
    notes = []
    if unbounded:
        notes.append(f"{unbounded} trades left out: the position opened has no bounded loss to measure R by")
    if still_open is not None:
        notes.append(f"the trade opened on {still_open} is still open at the last day's close: left out")
    return trades, errors, notes


def trade_columns(trades: list[Trade]) -> dict[str, Any]:
    """Trades as the ledger keeps them, a column a field (ledger.trial_trades gives them back as rows)."""
    return {"opened": [t.day.isoformat() for t in trades], "days": [t.days for t in trades],
            "net": [t.net for t in trades], "risk": [t.risk for t in trades], "r": [t.r for t in trades],
            "hedge_fills": [t.hedge_fills for t in trades],
            "attribution": {k: [t.attribution[k] for t in trades] for k in ATTRIBUTION},
            "fees": [t.fees for t in trades], "entry_cost": [t.entry_cost for t in trades],
            "exit_cost": [t.exit_cost for t in trades], "settled": [t.settled for t in trades]}
