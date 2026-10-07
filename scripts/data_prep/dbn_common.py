"""Shared helpers for turning Databento DBN quote records into OTrader Parquet rows."""

from __future__ import annotations

from datetime import date, datetime, time, timezone
from zoneinfo import ZoneInfo

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq

ET = ZoneInfo("America/New_York")
RTH_START = time(9, 30)  # US regular session, inclusive on both ends
RTH_END = time(16, 0)
UNDEF_PRICE = 2**63 - 1  # Databento's "no price" sentinel (fixed-point int64 max)
PRICE_SCALE = 1e9  # DBN prices are fixed-point, 1e-9 units

# Exact Parquet schema the OTrader backtester reads (see data/README.md).
TS_DTYPE = pl.Datetime(time_unit="us", time_zone="America/New_York")
OPTION_SCHEMA: dict[str, pl.DataType] = {
    "symbol": pl.String,
    "ts_recv": TS_DTYPE,
    "bid_px": pl.Float64,
    "ask_px": pl.Float64,
    "bid_sz": pl.Int64,
    "ask_sz": pl.Int64,
    "underlying_bid_px": pl.Float64,
    "underlying_ask_px": pl.Float64,
    "underlying_bid_sz": pl.Int64,
    "underlying_ask_sz": pl.Int64,
}


# The same schema as Arrow types, used when writing. Written through pyarrow so `symbol` is
# `string`: polars would write `large_string`, which older OTrader loaders silently skipped
# (every option dropped, backtest "ok" with no trades). Row groups use pyarrow's default size,
# matching the existing data/SPXW files.
OPTION_ARROW_SCHEMA = pa.schema(
    [
        ("symbol", pa.string()),
        ("ts_recv", pa.timestamp("us", tz="America/New_York")),
        ("bid_px", pa.float64()),
        ("ask_px", pa.float64()),
        ("bid_sz", pa.int64()),
        ("ask_sz", pa.int64()),
        ("underlying_bid_px", pa.float64()),
        ("underlying_ask_px", pa.float64()),
        ("underlying_bid_sz", pa.int64()),
        ("underlying_ask_sz", pa.int64()),
    ]
)


def write_option_day(df: pl.DataFrame, path) -> None:
    pq.write_table(df.to_arrow().cast(OPTION_ARROW_SCHEMA), path)


def scale_price(x) -> float | None:
    """Fixed-point DBN price -> float; None for missing / UNDEF."""
    if x is None or isinstance(x, bool) or not isinstance(x, (int, float)):
        return None
    if x == UNDEF_PRICE:
        return None
    return x / PRICE_SCALE


def ns_to_et(ns: int) -> datetime:
    return datetime.fromtimestamp(ns / 1e9, tz=timezone.utc).astimezone(ET)


def in_rth(dt_et: datetime) -> bool:
    return RTH_START <= dt_et.timetz().replace(tzinfo=None) <= RTH_END


def top_of_book(rec) -> tuple[float | None, float | None, int | None, int | None]:
    """(bid_px, ask_px, bid_sz, ask_sz) from the first book level of a BBO / CBBO record."""
    levels = getattr(rec, "levels", None) or []
    if not levels:
        return None, None, None, None
    lev = levels[0]
    return (
        scale_price(getattr(lev, "bid_px", None)),
        scale_price(getattr(lev, "ask_px", None)),
        getattr(lev, "bid_sz", None),
        getattr(lev, "ask_sz", None),
    )


def parse_day(s: str | None) -> date | None:
    return date.fromisoformat(s) if s else None
