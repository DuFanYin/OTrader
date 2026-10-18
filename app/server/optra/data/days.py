"""Backtest days: the Parquet files the backtester reads, one per symbol and trading day, in the
layout OUT/<SYMBOL>/<SYMBOL>-YYYY-MM/YYYYMMDD.parquet and the schema in doc/data.md.
Every day written (synthetic_days, recorded_days, the command lines in scripts/data) goes through conform,
day_path and write_option_day (convert_frame for options and underlying quoted apart): the same checks and the same
layout."""

from __future__ import annotations

import functools
import json
import re
from datetime import date, time
from pathlib import Path
from typing import NamedTuple
from zoneinfo import ZoneInfo

import numpy as np
import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq

ET = ZoneInfo("America/New_York")
RTH_START = time(9, 30)  # US regular session, inclusive on both ends
RTH_END = time(16, 0)

# The exact schema the backtester reads. Time in nanoseconds, as the engine keeps it:
# a coarser source widens to it without loss.
TS_DTYPE = pl.Datetime(time_unit="ns", time_zone="America/New_York")
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
# `string`: polars would write `large_string`, which older Optra loaders silently skipped
# (every option dropped, backtest "ok" with no trades).
OPTION_ARROW_SCHEMA = pa.schema(
    [
        ("symbol", pa.string()),
        ("ts_recv", pa.timestamp("ns", tz="America/New_York")),
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


OPEN_INTEREST = pa.field("open_interest", pa.int64())  # a day may carry it: each contract's, as of the open
PRICE_PLACES = range(2, 10)  # decimal places tried, fewest first: cents, ..., nanodollars (as some vendors send)
DECIMAL_PRICES = ("bid_px", "ask_px")  # the underlying's stay doubles: SPXW's comes from parity, not cents
_TEN_TO_MINUS = [10.0 ** -k for k in range(19)]


def decimal_prices(prices: pa.Array) -> pa.Array:
    """The prices as decimals (an integer and its places, as the data vendors send a price) of the fewest places
    in PRICE_PLACES that hold every one of them exactly, read back as the integer times 10^-places: the double it
    is now (9332 * 0.01 is 93.32000000000001, as the vendors' own clients make it). Unchanged (doubles) when no
    number of places does."""
    n = len(prices)
    valid = ~np.asarray(prices.is_null(), dtype=bool)
    values = np.asarray(prices.fill_null(0.0), dtype=np.float64)
    for places in PRICE_PLACES:
        integers = np.rint(values * 10.0**places)
        if np.all(np.abs(integers) < 10**17) and np.array_equal(integers[valid] * _TEN_TO_MINUS[places], values[valid]):
            break
    else:
        return prices
    words = np.zeros((n, 2), dtype=np.int64)  # a decimal128: a little-endian two's complement integer
    words[:, 0] = np.where(valid, integers, 0).astype(np.int64)
    words[:, 1] = np.where(words[:, 0] < 0, -1, 0)
    validity = pa.array(valid, pa.bool_()).buffers()[1]
    return pa.Array.from_buffers(pa.decimal128(18, places), n, [validity, pa.py_buffer(words.tobytes())],
                                 null_count=int(n - valid.sum()))


def doubles(prices: pa.Array) -> np.ndarray:
    """A price column as doubles, NaN where null: a decimal as its integer times 10^-places (as the engine reads it)."""
    if not pa.types.is_decimal(prices.type):
        return np.asarray(prices.fill_null(float("nan")), dtype=np.float64)
    prices = prices.cast(pa.decimal128(18, prices.type.scale)) if prices.type.bit_width != 128 else prices
    prices = pa.concat_arrays([prices]) if prices.offset else prices
    words = np.frombuffer(prices.buffers()[1], dtype=np.int64)[: 2 * len(prices)]
    out = words[0::2].astype(np.float64) * _TEN_TO_MINUS[prices.type.scale]
    out[np.asarray(prices.is_null(), dtype=bool)] = np.nan
    return out


def read_option_day(path, columns: list[str] | None = None) -> pl.DataFrame:
    """A day file as the backtester reads it, every price a double (OPTION_SCHEMA): a decimal price as its integer
    times 10^-places; null where the file has null."""
    return as_doubles(pq.read_table(path, columns=columns))


def as_doubles(table: pa.Table) -> pl.DataFrame:
    """A day file's rows (or a row group's) with every decimal price a double, as read_option_day gives them."""
    for i, field in enumerate(table.schema):
        if pa.types.is_decimal(field.type):
            prices = table.column(i).combine_chunks()
            table = table.set_column(i, field.name, pa.array(doubles(prices), pa.float64(), mask=np.asarray(prices.is_null(), dtype=bool)))
    return pl.from_arrow(table)


RISK_FREE_CURVE = b"risk_free_curve"  # a day file's metadata: the risk-free rates it is priced with


def risk_free_curve(years: list[float], rates: list[float]) -> dict:
    """A day's risk-free curve as a file carries it: rates continuously compounded, a year, at maturities in years
    (increasing); the engine takes a straight line between them and the end ones beyond."""
    if not years or len(years) != len(rates) or any(b <= a for a, b in zip(years, years[1:])):
        raise ConversionError(f"not a curve: years {years}, rates {rates}")
    if not all(np.isfinite(years)) or not all(np.isfinite(rates)):
        raise ConversionError(f"not a curve: years {years}, rates {rates}")
    return {"years": [float(y) for y in years], "rates": [float(r) for r in rates]}


def write_option_day(df: pl.DataFrame, path, rates: dict | None = None) -> None:
    """One day's file, the smallest that loses nothing (local/06.2_CONVERSION.md): rows by contract (symbol, then
    time), one row group an expiry (a backtest reads only the expiries it asks for); the option's prices as decimals
    when that is exact (decimal_prices), they and the time delta-encoded, the other columns by dictionary; zstd 9.
    rates (risk_free_curve): the day's risk-free curve, kept in the file's metadata; none: the backtest prices it at
    the rate it is given. Read back and compared with df value for value, in that order (and the curve), before it
    replaces anything: the file holds exactly what df holds, or this raises and writes nothing."""
    path = Path(path)
    schema = OPTION_ARROW_SCHEMA.append(OPEN_INTEREST) if OPEN_INTEREST.name in df.columns else OPTION_ARROW_SCHEMA
    if rates is not None:
        schema = schema.with_metadata({RISK_FREE_CURVE: json.dumps(risk_free_curve(rates["years"], rates["rates"]))})
    df = df.select(schema.names).sort("symbol", "ts_recv", maintain_order=True)
    table = df.to_arrow().cast(schema)
    for name in DECIMAL_PRICES:
        i = table.schema.get_field_index(name)
        table = table.set_column(i, name, decimal_prices(table.column(i).combine_chunks()))
    table = table.replace_schema_metadata(schema.metadata)
    delta = ["ts_recv"] + [c for c in DECIMAL_PRICES if pa.types.is_decimal(table.schema.field(c).type)]
    writing = path.with_suffix(".writing")
    expiries = pa.compute.utf8_slice_codeunits(table.column("symbol"), 0, 6).to_numpy(zero_copy_only=False)
    starts = [0, *np.flatnonzero(expiries[1:] != expiries[:-1]) + 1, len(expiries)]
    with pq.ParquetWriter(writing, table.schema, compression="zstd", compression_level=9,
                          use_dictionary=[c for c in table.schema.names if c not in delta],
                          column_encoding={c: "DELTA_BINARY_PACKED" for c in delta},
                          store_decimal_as_integer=True) as writer:
        for first, end in zip(starts, starts[1:]):
            writer.write_table(table.slice(first, end - first), row_group_size=end - first)
    del table
    # Read back a row group (an expiry) at a time, to keep the memory near the rows given.
    written = pq.ParquetFile(writing)
    same = (written.metadata.num_row_groups == len(starts) - 1
            and (written.schema_arrow.metadata or {}).get(RISK_FREE_CURVE) == (schema.metadata or {}).get(RISK_FREE_CURVE)
            and all(as_doubles(written.read_row_group(g)).equals(df.slice(first, end - first))
                    for g, (first, end) in enumerate(zip(starts, starts[1:]))))
    if not same:
        writing.unlink()
        raise ConversionError(f"{path}: the file written does not read back as the rows given")
    writing.replace(path)


OCC_BODY = re.compile(
    r"^\d{6}[CP]\d{8}$"
)  # YYMMDD + C/P + strike*1000, as parse_occ_symbol expects


class ConversionError(ValueError):
    """A DBN file cannot be converted (bad symbols, missing underlying rows, ...)."""


def load_underlying_frame(dfu: pl.DataFrame, source: str = "underlying frame") -> pl.DataFrame:
    """Underlying quotes (ts_recv, bid_px, ask_px, bid_sz, ask_sz) -> ts_recv + underlying_{bid,ask}_{px,sz}, one row per timestamp."""
    missing = {"ts_recv", "bid_px", "ask_px", "bid_sz", "ask_sz"} - set(dfu.columns)
    if missing:
        raise ConversionError(f"{source} is missing columns {sorted(missing)}")
    return (
        dfu.select("ts_recv", "bid_px", "ask_px", "bid_sz", "ask_sz")
        .with_columns(pl.col("ts_recv").cast(TS_DTYPE))
        .unique(subset=["ts_recv"], keep="first")
        .sort("ts_recv")
        .rename({c: f"underlying_{c}" for c in ("bid_px", "ask_px", "bid_sz", "ask_sz")})
    )


def attach_underlying(df_opt: pl.DataFrame, dfu: pl.DataFrame) -> pl.DataFrame:
    missing = (
        df_opt.select("ts_recv").unique().join(dfu.select("ts_recv"), on="ts_recv", how="anti")
    )
    if len(missing):
        sample = ", ".join(str(t) for t in missing["ts_recv"].sort().head(5).to_list())
        raise ConversionError(
            f"{len(missing):,} option timestamps have no underlying quote (e.g. {sample})"
        )
    return df_opt.join(dfu, on="ts_recv", how="left")


def conform(df: pl.DataFrame) -> pl.DataFrame:
    """Exact column order / dtypes of the backtester schema, open_interest kept when the frame has it.

    Rejects symbols the loader cannot parse.
    """
    bad = df.filter(~pl.col("symbol").str.contains(OCC_BODY.pattern))["symbol"].unique()
    if len(bad):
        raise ConversionError(
            f"{len(bad)} symbols are not OCC 'YYMMDD[C|P]strike*1000', e.g. {bad.head(3).to_list()}"
        )
    extra = [pl.col(OPEN_INTEREST.name).cast(pl.Int64)] if OPEN_INTEREST.name in df.columns else []
    return df.select([pl.col(c).cast(t) for c, t in OPTION_SCHEMA.items()] + extra)


def split_by_day(df: pl.DataFrame) -> dict[date, pl.DataFrame]:
    df = df.with_columns(pl.col("ts_recv").dt.date().alias("_day"))
    return {
        day: part.drop("_day")
        for (day,), part in df.partition_by("_day", as_dict=True, maintain_order=True).items()
    }


DAY_FILE = re.compile(r"(\d{8})\.parquet$")


def day_of(path: Path | str) -> date | None:
    """The trading day a day file holds, from its name (YYYYMMDD.parquet)."""
    m = DAY_FILE.search(Path(path).name)
    if not m:
        return None
    d = m.group(1)
    return date(int(d[:4]), int(d[4:6]), int(d[6:]))


def held_day(path: Path | str) -> date | None:
    """The trading day a day file holds, from its own quotes' times (the last row group's latest, in New York; the
    file's footer, not its rows), whatever its name; None for a file with no times."""
    st = Path(path).stat()
    return _held_day(str(path), st.st_size, st.st_mtime_ns)


@functools.lru_cache(maxsize=65536)
def _held_day(path: str, size: int, mtime_ns: int) -> date | None:
    meta = pq.read_metadata(path)
    if meta.num_row_groups == 0:
        return None
    group = meta.row_group(meta.num_row_groups - 1)
    for i in range(group.num_columns):
        column = group.column(i)
        if column.path_in_schema == "ts_recv" and column.statistics is not None and column.statistics.has_min_max:
            latest = column.statistics.max
            return latest.astimezone(ET).date() if hasattr(latest, "astimezone") else None
    return None


class OptionSymbol(NamedTuple):
    root: str
    expiry: date
    right: str  # "C" or "P"
    strike: float


def parse_option(symbol: str) -> OptionSymbol | None:
    """The engine's option symbol, "SPXW-20261013-C-7820", read; None for anything else (shares)."""
    parts = symbol.split("-")
    if len(parts) != 4 or parts[2] not in ("C", "P") or len(parts[1]) != 8 or not parts[1].isdigit():
        return None
    try:
        d = parts[1]
        return OptionSymbol(parts[0], date(int(d[:4]), int(d[4:6]), int(d[6:])), parts[2], float(parts[3]))
    except ValueError:
        return None


def day_path(out_root: Path, symbol: str, day: date) -> Path:
    return out_root / symbol / f"{symbol}-{day:%Y-%m}" / f"{day:%Y%m%d}.parquet"


def convert_frame(
    df_opt: pl.DataFrame, dfu: pl.DataFrame, out_root: Path, symbol: str, overwrite: bool, rates: dict | None = None
) -> list[Path]:
    """Joined, validated, per-day files (each with the risk-free curve rates, when given). Checks every target before
    writing any."""
    days = split_by_day(conform(attach_underlying(df_opt, dfu)))
    targets = {day: day_path(out_root, symbol, day) for day in days}
    existing = [p for p in targets.values() if p.exists()]
    if existing and not overwrite:
        raise ConversionError(f"{existing[0]} already exists (pass --overwrite to replace)")
    for day, part in days.items():
        targets[day].parent.mkdir(parents=True, exist_ok=True)
        write_option_day(part, targets[day], rates)
    return list(targets.values())
