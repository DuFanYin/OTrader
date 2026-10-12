"""Synthetic option days in the backtest Parquet layout, from a model whose answers are known.

The underlying follows a geometric Brownian motion minute by minute through the regular session
(09:30-16:00 ET) of every weekday, with a gap between sessions. Each day lists the nearest
`expiries` daily expiries and `strikes` strikes either side of the day's open, and every option
is quoted every minute around its Black price at the implied vol the arguments set. The price
is computed the way Optra inverts it for IV (Black on the forward, here the underlying mid
grown at rate, discounted at rate; time to the 16:00 New York close of the expiry date), so
with tick 0 and spread 0 a backtester at the same risk-free rate (both default to 0.05) reads
back exactly the implied vol put in, and the forward it finds by put-call parity.

Vols are annualised over calendar time, as Optra's are (time to expiry in calendar years).
Knobs that plant something for a strategy to find:
  implied_vol above realized_vol   options are rich: selling them and hedging earns the gap
  skew, smile                      a volatility smile in the quotes
  momentum M                       the last 30 minutes drift by M x the first 30 minutes' return
                                   (intraday momentum)

Made by POST /api/data/synthetic, or from the command line by scripts/data/synthetic_to_parquet.py.
Writes OUT/<SYMBOL>/<SYMBOL>-YYYY-MM/YYYYMMDD.parquet through convert_frame
(the same checks and layout as real data) and OUT/<SYMBOL>/synthetic-<start>-<end>.json with the
arguments. The same arguments always give the same files. Weekends are skipped; holidays are not.
"""

from __future__ import annotations

import json
import math
import random
from dataclasses import asdict, dataclass
from datetime import date, datetime, time, timedelta
from pathlib import Path
from statistics import NormalDist

import polars as pl

from optra.services.days import (
    ET,
    RTH_END,
    RTH_START,
    TS_DTYPE,
    ConversionError,
    convert_frame,
    day_path,
    load_underlying_frame,
)

YEAR_SECONDS = 365.25 * 24 * 3600  # model::years_to_expiry (engine/model/utility.cpp)
MIN_T = 1e-6  # years_to_expiry's floor for a positive time
MIN_IV, MAX_IV = 0.01, 3.0  # the smile is clipped to this range
MOMENTUM_MINUTES = 30
STD_NORMAL = NormalDist()


@dataclass(frozen=True)
class Model:
    spot: float = 600.0
    realized_vol: float = 0.15
    overnight_vol: float | None = None  # None: the same as realized_vol
    drift: float = 0.0
    rate: float = 0.05  # the forward is the spot grown at it, and quotes are discounted at it
    implied_vol: float = 0.15
    skew: float = 0.0
    smile: float = 0.0
    momentum: float = 0.0
    strike_step: float = 1.0
    strikes: int = 20
    expiries: int = 6
    tick: float = 0.01
    spread: float = 0.02
    size: int = 100
    seed: int = 1


# ── time ──────────────────────────────────────────────────────────────────────


def trading_days(start: date, end: date) -> list[date]:
    days = (start + timedelta(n) for n in range((end - start).days + 1))
    return [d for d in days if d.weekday() < 5]


def session_minutes(day: date) -> list[datetime]:
    """Every minute of the regular session, both ends included (dbn_common.regular_session)."""
    first = datetime.combine(day, RTH_START, tzinfo=ET)
    n = (RTH_END.hour - RTH_START.hour) * 60 + RTH_END.minute - RTH_START.minute
    return [first + timedelta(minutes=i) for i in range(n + 1)]


def engine_expiry(day: date) -> datetime:
    """The instant Optra takes as the expiry of an option expiring on `day`: the 16:00 New York
    close it settles on (utilities::option_expiry_time)."""
    return datetime.combine(day, time(16, 0), tzinfo=ET)


def years_to_expiry(now: datetime, expiry: datetime) -> float:
    # timestamps, not datetime subtraction: two datetimes in the same zone subtract by wall clock,
    # which skips the hour a daylight-saving change adds or removes
    secs = int(expiry.timestamp() - now.timestamp())
    return 0.0 if secs <= 0 else max(MIN_T, secs / YEAR_SECONDS)


# ── pricing ───────────────────────────────────────────────────────────────────


def black(forward: float, strike: float, vol: float, t: float, is_call: bool) -> float:
    """Undiscounted Black price; intrinsic value at t = 0."""
    if t <= 0 or vol <= 0:
        return max(forward - strike, 0.0) if is_call else max(strike - forward, 0.0)
    s = vol * math.sqrt(t)
    d1 = math.log(forward / strike) / s + s / 2
    if is_call:
        return forward * STD_NORMAL.cdf(d1) - strike * STD_NORMAL.cdf(d1 - s)
    return strike * STD_NORMAL.cdf(s - d1) - forward * STD_NORMAL.cdf(-d1)


def price(m: Model, spot: float, strike: float, t: float, is_call: bool) -> float:
    """The option's fair price: Black on the forward spot·e^(rate·t), discounted at rate."""
    forward = spot * math.exp(m.rate * t)
    vol = smile_vol(m, forward, strike, t)
    return math.exp(-m.rate * t) * black(forward, strike, vol, t, is_call)


def smile_vol(m: Model, forward: float, strike: float, t: float) -> float:
    """ATM vol + skew·z + smile·z², z = ln(K/F) in standard deviations of the ATM vol."""
    if t <= 0 or (m.skew == 0 and m.smile == 0):
        return m.implied_vol
    z = math.log(strike / forward) / (m.implied_vol * math.sqrt(t))
    return min(MAX_IV, max(MIN_IV, m.implied_vol + m.skew * z + m.smile * z * z))


def quote(price: float, m: Model) -> tuple[float, float]:
    """Bid and ask around `price`: a spread of `spread` × price, at least one tick, on the tick."""
    half = max(m.tick, m.spread * price) / 2
    if m.tick <= 0:
        return max(price - half, 0.0), price + half
    bid = math.floor(round((price - half) / m.tick, 9)) * m.tick
    ask = math.ceil(round((price + half) / m.tick, 9)) * m.tick
    return round(max(bid, 0.0), 10), round(max(ask, m.tick), 10)


def underlying_quote(price: float, m: Model) -> tuple[float, float]:
    """One tick wide; bid = ask = price with no tick."""
    if m.tick <= 0:
        return price, price
    bid = math.floor(round(price / m.tick, 9)) * m.tick
    return round(bid, 10), round(bid + m.tick, 10)


def occ_body(expiry: date, is_call: bool, strike: float) -> str:
    return f"{expiry:%y%m%d}{'C' if is_call else 'P'}{round(strike * 1000):08d}"


# ── simulation ────────────────────────────────────────────────────────────────


def normal(rng: random.Random) -> float:
    u = rng.random()
    while u == 0.0:
        u = rng.random()
    return STD_NORMAL.inv_cdf(u)


def gbm_step(price: float, vol: float, drift: float, years: float, z: float) -> float:
    return price * math.exp((drift - vol * vol / 2) * years + vol * math.sqrt(years) * z)


def simulate_session(m: Model, rng: random.Random, open_px: float, minutes: int) -> list[float]:
    """The underlying at each minute of a session that opens at `open_px`."""
    step = 60 / YEAR_SECONDS
    path = [open_px]
    for i in range(1, minutes):
        drift = m.drift
        if m.momentum and i > minutes - 1 - MOMENTUM_MINUTES:
            first = math.log(path[MOMENTUM_MINUTES] / path[0])
            drift += m.momentum * first / (MOMENTUM_MINUTES * step)
        path.append(gbm_step(path[-1], m.realized_vol, drift, step, normal(rng)))
    return path


def next_open(m: Model, rng: random.Random, close_px: float, close: datetime, open_: datetime):
    vol = m.realized_vol if m.overnight_vol is None else m.overnight_vol
    years = (open_.timestamp() - close.timestamp()) / YEAR_SECONDS
    return gbm_step(close_px, vol, m.drift, years, normal(rng))


# ── one day ───────────────────────────────────────────────────────────────────


def chain(m: Model, day: date, open_px: float, days: list[date]) -> list[tuple[date, float]]:
    """(expiry, strike) listed on `day`: the nearest expiries, strikes around the open."""
    expiries = [d for d in days if d >= day][: m.expiries]
    center = round(open_px / m.strike_step) * m.strike_step
    strikes = [center + i * m.strike_step for i in range(-m.strikes, m.strikes + 1)]
    return [(e, round(k, 6)) for e in expiries for k in strikes if k > 0]


def day_frames(m: Model, day: date, path: list[float], listed) -> tuple[pl.DataFrame, pl.DataFrame]:
    """(option quotes, underlying quotes) for one session."""
    stamps = session_minutes(day)
    und = {"ts_recv": stamps, "bid_px": [], "ask_px": [], "bid_sz": [], "ask_sz": []}
    cols = {k: [] for k in ("symbol", "ts_recv", "bid_px", "ask_px", "bid_sz", "ask_sz")}
    contracts = [
        (occ_body(e, c, k), engine_expiry(e), k, c) for e, k in listed for c in (True, False)
    ]
    for ts, px in zip(stamps, path):
        u_bid, u_ask = underlying_quote(px, m)
        und["bid_px"].append(u_bid)
        und["ask_px"].append(u_ask)
        und["bid_sz"].append(m.size)
        und["ask_sz"].append(m.size)
        mid = (u_bid + u_ask) / 2
        for symbol, expiry, strike, is_call in contracts:
            t = years_to_expiry(ts, expiry)
            bid, ask = quote(price(m, mid, strike, t, is_call), m)
            cols["symbol"].append(symbol)
            cols["ts_recv"].append(ts)
            cols["bid_px"].append(bid)
            cols["ask_px"].append(ask)
            cols["bid_sz"].append(m.size)
            cols["ask_sz"].append(m.size)
    utc = pl.Datetime("us", "UTC")
    to_et = pl.col("ts_recv").dt.convert_time_zone("America/New_York").cast(TS_DTYPE)
    options = pl.DataFrame(
        cols,
        schema={
            "symbol": pl.String,
            "ts_recv": utc,
            "bid_px": pl.Float64,
            "ask_px": pl.Float64,
            "bid_sz": pl.Int64,
            "ask_sz": pl.Int64,
        },
    ).with_columns(to_et)
    underlying = pl.DataFrame(
        und,
        schema={
            "ts_recv": utc,
            "bid_px": pl.Float64,
            "ask_px": pl.Float64,
            "bid_sz": pl.Int64,
            "ask_sz": pl.Int64,
        },
    ).with_columns(to_et)
    return options, load_underlying_frame(underlying, source="synthetic underlying")


# ── all days ──────────────────────────────────────────────────────────────────


def generate(m: Model, start: date, end: date, out_root: Path, symbol: str, overwrite: bool):
    """Writes every weekday from start to end; returns the files written. Checks every target
    before writing any."""
    rng = random.Random(m.seed)
    days = trading_days(start, end + timedelta(days=7 * m.expiries))  # expiries past `end`
    session_days = [d for d in days if d <= end]
    if not session_days:
        raise ValueError(f"no weekday from {start} to {end}")
    existing = [p for d in session_days if (p := day_path(out_root, symbol, d)).exists()]
    if existing and not overwrite:
        raise ConversionError(f"{existing[0]} already exists (pass --overwrite to replace)")
    written, open_px, last_close = [], m.spot, None
    for day in session_days:
        stamps = session_minutes(day)
        if last_close is not None:
            open_px = next_open(m, rng, last_close[1], last_close[0], stamps[0])
        path = simulate_session(m, rng, open_px, len(stamps))
        options, underlying = day_frames(m, day, path, chain(m, day, open_px, days))
        written += convert_frame(options, underlying, out_root, symbol, overwrite=True)
        last_close = (stamps[-1], path[-1])
    sidecar = out_root / symbol / f"synthetic-{start:%Y%m%d}-{end:%Y%m%d}.json"
    sidecar.write_text(
        json.dumps(
            {
                "generator": Path(__file__).name,
                "symbol": symbol,
                "start": str(start),
                "end": str(end),
                **asdict(m),
            },
            indent=2,
        )
    )
    return written


def check(m: Model) -> None:
    bad = [f for f in ("spot", "implied_vol", "strike_step") if getattr(m, f) <= 0]
    bad += [f for f in ("strikes", "expiries", "size") if getattr(m, f) < 1]
    bad += [f for f in ("realized_vol", "tick", "spread") if getattr(m, f) < 0]
    if m.overnight_vol is not None and m.overnight_vol < 0:
        bad.append("overnight_vol")
    if bad:
        raise ValueError(f"out of range: {', '.join(bad)}")
