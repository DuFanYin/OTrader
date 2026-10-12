"""Strategies over backtest days, trade by trade, by the promotion measures (POST /api/backtest/report).

Each strategy runs over every day given (engine/build/entry_backtest --files, the days in parallel),
and each day's fills come back from the backtest. A strategy that opens one position a day and
closes it the same day (the 0DTE buyers, ComboStrategy) makes one trade a day: its result is the
day's P&L after fees, its risk what the position it opened can lose at most at expiry (for an
option bought, the premium paid; a structure with legs on several expiries, or one that can lose
without bound: what it paid, when it paid), and R = result / risk. The day's P&L by source (the
engine's attribution: delta, gamma, theta, vega, other, fills) is summed per strategy. Reported, apart for the research days and the holdout
(holdout_from), and for all: trades, win rate, average win and loss in R, payoff (average win /
average loss, in USD), expectancy (mean R) with its t (mean / sd x sqrt(n)), net USD, the longest
losing streak, the deepest drawdown (USD, cumulative), and the gap to the control
(RandomBuyStrategy) when it is run.

A strategy needs at least 300 independent trades before it is promoted; fewer, and the numbers say
what to look at, not what works.
"""

from __future__ import annotations

import json
import math
import re
import statistics
import subprocess
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import Any

from optra.utils.files import inside_data, repo_root

OPTION = re.compile(r"-\d{8}-[CP]-")
DAY = re.compile(r"(\d{8})\.parquet$")
COLUMNS = ["trades", "win rate", "avg win R", "avg loss R", "payoff", "expectancy R", "t",
           "net USD", "losing streak", "drawdown USD"]


ATTRIBUTION = ("delta", "gamma", "theta", "vega", "other", "fills")


@dataclass
class Trade:
    day: date
    risk: float  # USD the position opened can lose at most (see the module's doc)
    net: float   # the day's P&L after fees
    attribution: dict[str, float]  # the day's P&L by source, before fees
    hedge_fills: int = 0  # fills in shares

    @property
    def r(self) -> float:
        return self.net / self.risk


def parse_option(symbol: str) -> tuple[str, str, float] | None:
    """"SPXW-20261013-C-7820" -> (expiry "20261013", "C" or "P", strike); None for shares."""
    parts = symbol.split("-")
    if len(parts) != 4 or parts[2] not in ("C", "P") or not OPTION.search(symbol):
        return None
    try:
        return parts[1], parts[2], float(parts[3])
    except ValueError:
        return None


def max_loss(legs: list[tuple[str, str, float, float, float]], multiplier: float = 100.0) -> float | None:
    """legs: (expiry, C or P, strike, contracts: bought +, sold -, price paid). What the position
    can lose at most: on one expiry, the worst of its payoff at expiry less what it paid (checked at
    0, at each strike and past the last one); otherwise, or when it can lose without bound, what it
    paid if it paid. None when there is no bound to give."""
    paid = sum(q * price * multiplier for _, _, _, q, price in legs)
    if len({e for e, _, _, _, _ in legs}) == 1:
        def value(s: float) -> float:
            return sum(q * multiplier * (max(s - k, 0.0) if right == "C" else max(k - s, 0.0))
                       for _, right, k, q, _ in legs)
        slope_above = sum(q * multiplier for _, right, _, q, _ in legs if right == "C")
        if slope_above >= 0:  # past the last strike it gains or holds
            worst = min(value(s) for s in [0.0, *(k for _, _, k, _, _ in legs)])
            loss = paid - worst
            return loss if loss > 0 else None
    return paid if paid > 0 else None


def days_of(path: Path) -> list[Path]:
    """A folder of backtest days (searched below) or one day, in date order."""
    files = sorted(path.rglob("*.parquet")) if path.is_dir() else [path]
    return sorted(files, key=lambda p: DAY.search(p.name).group(1) if DAY.search(p.name) else p.name)


def run(files: list[Path], spec: str, fee_rate: float, risk: list[str],
        slippage_bps: float | None = None) -> tuple[list[Trade], list]:
    """spec: a class, or "Class:name=value,name=value". The trades, and the backtest's errors (and
    a note of the days left out: a position opened with no bounded loss has no R)."""
    cls, _, params = spec.partition(":")
    settings = [p for p in params.split(",") if p]
    entry = repo_root() / "engine" / "build" / "entry_backtest"
    cmd = [str(entry), "--files", *map(str, files), cls, "--fee-rate", f"{fee_rate:g}", *settings,
           *risk]
    if slippage_bps is not None:
        cmd += ["--slippage-bps", f"{slippage_bps:g}"]
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=6 * 3600)
    if done.returncode != 0:
        raise ValueError(f"{spec}: entry_backtest failed ({done.returncode}): "
                         f"{(done.stderr or done.stdout).strip()[-500:]}")
    result = json.loads(done.stdout)
    trades = []
    unbounded = 0
    for daily in result.get("daily_results", []):
        m = DAY.search(daily["file"])
        if not m or not daily.get("trades"):
            continue
        options = [t for t in daily["trades"] if parse_option(t["symbol"])]
        if not options:
            continue
        opened = [t for t in options if t["orderid"] == options[0]["orderid"]]  # the first order
        legs = []
        for t in opened:
            expiry, right, strike = parse_option(t["symbol"])
            q = t["volume"] if t["direction"] == "LONG" else -t["volume"]
            legs.append((expiry, right, strike, q, t["price"]))
        risk = max_loss(legs)
        if risk is None:
            unbounded += 1
            continue
        day = date(int(m.group(1)[:4]), int(m.group(1)[4:6]), int(m.group(1)[6:]))
        attribution = {k: float((daily.get("attribution") or {}).get(k, 0.0)) for k in ATTRIBUTION}
        trades.append(Trade(day, risk, daily["net_pnl"], attribution,
                            sum(1 for t in daily["trades"] if not parse_option(t["symbol"]))))
    errors = result.get("errors", [])[:3]
    if unbounded:
        errors.append(f"{unbounded} days left out: the position opened has no bounded loss to measure R by")
    return trades, errors


def per_day(trades: list[Trade]) -> list[dict[str, Any]]:
    """Each trade: its day, net after fees, risk, R, share fills and P&L by source."""
    return [{"day": t.day.isoformat(), "net": round(t.net, 2), "risk": round(t.risk, 2), "r": round(t.r, 4),
             "hedge_fills": t.hedge_fills, "attribution": {k: round(v, 2) for k, v in t.attribution.items()}}
            for t in trades]


def measures(trades: list[Trade]) -> dict[str, Any]:
    n = len(trades)
    if n == 0:
        return {"trades": 0}
    rs = [t.r for t in trades]
    wins = [t for t in trades if t.net > 0]
    losses = [t for t in trades if t.net <= 0]
    mean = statistics.fmean(rs)
    sd = statistics.stdev(rs) if n > 1 else 0.0
    streak = longest = 0
    equity = peak = drawdown = 0.0
    for t in trades:
        streak = streak + 1 if t.net <= 0 else 0
        longest = max(longest, streak)
        equity += t.net
        peak = max(peak, equity)
        drawdown = min(drawdown, equity - peak)
    avg_win = statistics.fmean(t.net for t in wins) if wins else 0.0
    avg_loss = statistics.fmean(t.net for t in losses) if losses else 0.0
    return {
        "trades": n,
        "win rate": f"{len(wins) / n:.0%}",
        "avg win R": f"{statistics.fmean(t.r for t in wins):+.2f}" if wins else "-",
        "avg loss R": f"{statistics.fmean(t.r for t in losses):+.2f}" if losses else "-",
        "payoff": f"{avg_win / abs(avg_loss):.2f}" if avg_loss else "-",
        "expectancy R": f"{mean:+.3f}",
        "t": f"{mean / sd * math.sqrt(n):+.2f}" if sd > 0 else "-",
        "net USD": f"{sum(t.net for t in trades):+,.0f}",
        "losing streak": longest,
        "drawdown USD": f"{drawdown:,.0f}",
        "expectancy": mean,
        "attribution": {k: round(sum(t.attribution[k] for t in trades), 2) for k in ATTRIBUTION},
        "fees": round(sum(sum(t.attribution.values()) - t.net for t in trades), 2),
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


def report(days: Path | list[Path], strategies: list[str], holdout_from: date | None, fee_rate: float,
           risk: list[str], slippage_bps: float | None = None) -> dict[str, Any]:
    """days: a folder of backtest days, or one (relative: to the repository), or the days
    themselves. risk: limits for the backtests, as risk.name=value."""
    if isinstance(days, list):
        files = [inside_data(d) for d in days]
    else:
        days = inside_data(days)
        files = days_of(days)
    if not files:
        raise ValueError(f"no backtest days under {days}")
    results, errors = {}, {}
    for spec in strategies:
        results[spec], errs = run(files, spec, fee_rate, risk, slippage_bps)
        if errs:
            errors[spec] = errs

    def period(trades: list[Trade], which: str) -> list[Trade]:
        if holdout_from is None or which == "all":
            return trades
        return [t for t in trades if (t.day >= holdout_from) == (which == "holdout")]

    header = (f"{len(files)} days, {files[0].name} to {files[-1].name}; fees {fee_rate:g} USD a "
              f"contract; R = a day's P&L after fees / what the position opened could lose at most")
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
            "trades": {spec: per_day(trades) for spec, trades in results.items()},
            "errors": errors, "markdown": markdown}
