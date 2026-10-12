"""The trading engine's days as option chains for the lab: a backtest day (data/<SYMBOL>/) or a day the
live engine recorded (data/recorded/<SYMBOL>/, converted into data/recorded-parquet/<SYMBOL>/ when its
CSV is newer: today's is as fresh as the last minute recorded), at one of its timestamps, in the chain shape the
screener and option_chain tools read (services/market_data asks this module for them).

Priced the way the engine prices a snapshot: each option's mark is its mid (one side when only one is
quoted); each expiry's forward comes from put-call parity at the strikes quoted both ways nearest the
money (model::PortfolioData::chain_forward: the median of the three with the smallest |C - P|, or the
underlying grown at the rate when that is off by more than 10%); the implied vol is Black on that
forward (quant::black_implied_vol, through the lab's C ABI: the engine's own function), time to the 16:00 New York close of the expiry
date. Prices are USD per share; a contract is 100.
"""

from __future__ import annotations

import math
import os
import statistics
import tempfile
import threading
from datetime import date, datetime, time
from pathlib import Path

import polars as pl

from optra.infra.numerics import black_batch
from optra.services.market_data import expiry_table

from optra.services import recorded_days
from optra.services.days import ET
from optra.utils import files

RATE = 0.05                 # entry_backtest's --risk-free-rate default
MULTIPLIER = 100.0
YEAR_SECONDS = 365.25 * 24 * 3600   # model::years_to_expiry
MIN_T = 1e-6
MAX_FORWARD_GAP = 0.1       # ChainData::kMaxForwardGap
PARITY_STRIKES = 3


def _mark(bid: float, ask: float) -> float:
    """model::mark: the mid, or the one side quoted."""
    if bid > 0 and ask > 0:
        return 0.5 * (bid + ask)
    return bid if bid > 0 else (ask if ask > 0 else 0.0)


def _years(now: datetime, expiry: date) -> float:
    secs = datetime.combine(expiry, time(16, 0), tzinfo=ET).timestamp() - now.timestamp()
    return 0.0 if secs <= 0 else max(MIN_T, secs / YEAR_SECONDS)


def _forward(pairs: list[tuple[float, float, float]], spot: float, years: float) -> float:
    """pairs: (strike, call mark, put mark) of the strikes quoted on both sides of both options."""
    grow = math.exp(RATE * years)
    nearest = sorted(pairs, key=lambda p: abs(p[1] - p[2]))[:PARITY_STRIKES]
    if nearest:
        implied = statistics.median(k + grow * (c - p) for k, c, p in nearest)
        if abs(implied / spot - 1.0) < MAX_FORWARD_GAP:
            return implied
    return spot * grow


_converting = threading.Lock()


def _refresh_recorded(symbol: str) -> None:
    """A day the live engine is still recording (its CSV newer than its Parquet, or no Parquet yet)
    is converted again, so the newest quotes are there; written beside and renamed into place, so a
    reader never sees half a file."""
    data = files.data_root()
    out = data / "recorded-parquet"
    with _converting:
        for csv in sorted((data / "recorded" / symbol).rglob("*.csv")) if (data / "recorded" / symbol).is_dir() else []:
            target = out / symbol / csv.parent.name / f"{csv.stem}.parquet"
            if target.exists() and target.stat().st_mtime >= csv.stat().st_mtime:
                continue
            out.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir=out) as tmp:
                written, _failed = recorded_days.convert([csv], Path(tmp), symbol, overwrite=True)
                for f in written:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    os.replace(f, target)


def _day_files(symbol: str) -> dict[str, tuple[Path, str]]:
    """ISO day -> (Parquet file, "backtest" or "recorded"); a recorded day wins over a backtest one."""
    _refresh_recorded(symbol)
    data = files.data_root()
    days: dict[str, tuple[Path, str]] = {}
    for root, kind in ((data / symbol, "backtest"), (data / "recorded-parquet" / symbol, "recorded")):
        for f in root.rglob("*.parquet") if root.is_dir() else []:
            if len(f.stem) == 8 and f.stem.isdigit():
                days[f"{f.stem[:4]}-{f.stem[4:6]}-{f.stem[6:]}"] = (f, kind)
    return days


def _snapshot_time(day: str, at: str | None, stamps: pl.Series) -> datetime:
    """The day's last timestamp at or before `at` (HH:MM or an ISO time, New York), or its first
    when `at` is before them all; None: the last."""
    if at is None:
        return stamps[-1]
    want = datetime.fromisoformat(at) if "T" in at else datetime.combine(date.fromisoformat(day), time.fromisoformat(at))
    want = want.replace(tzinfo=ET) if want.tzinfo is None else want.astimezone(ET)
    if want.date().isoformat() != day:
        raise ValueError(f"{at} is not on {day}")
    before = stamps.filter(stamps <= want)
    return before[-1] if len(before) else stamps[0]


def load(symbol: str, day: str | None, at: str | None) -> dict:
    days = _day_files(symbol)
    if not days:
        raise ValueError(f"no {symbol} days")
    day = day or max(days)
    if day not in days:
        raise ValueError(f"no {symbol} day {day}")
    path, kind = days[day]
    frame = pl.scan_parquet(path)
    stamps = frame.select(pl.col("ts_recv").unique().sort()).collect()["ts_recv"]
    now = _snapshot_time(day, at, stamps)
    rows = frame.filter(pl.col("ts_recv") == now).collect()
    if rows.is_empty():
        raise ValueError(f"no quotes on {day} at {now}")
    first = rows.row(0, named=True)
    spot = _mark(first["underlying_bid_px"] or 0.0, first["underlying_ask_px"] or 0.0)

    by_expiry: dict[date, dict[float, dict[str, tuple[float, float]]]] = {}
    for sym, bid, ask in rows.select("symbol", "bid_px", "ask_px").iter_rows():
        expiry = date(2000 + int(sym[:2]), int(sym[2:4]), int(sym[4:6]))
        by_expiry.setdefault(expiry, {}).setdefault(int(sym[7:15]) / 1000.0, {})[sym[6]] = (bid or 0.0, ask or 0.0)  # null: not quoted

    chain: list[dict] = []
    for expiry in sorted(by_expiry):
        years = _years(now, expiry)
        if years <= 0:
            continue
        strikes = by_expiry[expiry]
        pairs = [(k, _mark(*q["C"]), _mark(*q["P"])) for k, q in strikes.items()
                 if "C" in q and "P" in q and min(*q["C"], *q["P"]) > 0]
        forward = _forward(pairs, spot, years)
        options = [(k, right, bid, ask) for k, q in sorted(strikes.items()) for right, (bid, ask) in sorted(q.items())]
        n = len(options)
        ivs = black_batch([o[1] == "C" for o in options], [forward] * n, [o[0] for o in options], [years] * n, RATE,
                          prices=[_mark(b, a) for _, _, b, a in options])["ivs"]
        expiry_ms = datetime.combine(expiry, time(16, 0), tzinfo=ET).timestamp() * 1000
        for (k, right, bid, ask), iv in zip(options, ivs, strict=False):
            chain.append({
                "symbol": f"{symbol} {expiry:%y%m%d}{right}{round(k * 1000):08d}",
                "option_type": "call" if right == "C" else "put",
                "expiry": expiry.isoformat(),
                "expiry_ts": expiry_ms,
                "strike": k,
                "forward": forward,
                "years": years,
                "bid": bid if bid > 0 else None,
                "ask": ask if ask > 0 else None,
                "mark": _mark(bid, ask),
                "iv": iv if iv and iv > 0 else 0.0,
                "volume": 0.0,
                "oi": 0.0,
            })
    return {
        "currency": symbol,
        "source": kind,                  # backtest | recorded
        "fetched_at": now.isoformat(),
        "spot": spot,
        "rate": RATE,
        "multiplier": MULTIPLIER,
        "n_instruments": len(chain),
        "expiries": expiry_table(chain, spot),
        "chain": chain,
    }


def symbols() -> list[str]:
    """The underlyings with backtest or recorded days."""
    data = files.data_root()
    found = set()
    for root in (data, data / "recorded-parquet", data / "recorded"):
        for d in root.iterdir() if root.is_dir() else []:
            if d.is_dir() and d.name.isupper() and any(next(d.rglob(g), None) for g in ("*.parquet", "*.csv")):
                found.add(d.name)
    return sorted(found)


def days(symbol: str) -> list[str]:
    """The ISO days there are for the symbol, backtest and recorded."""
    return sorted(_day_files(symbol))
