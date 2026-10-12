"""Backtest days: the Parquet files the backtester reads, one per symbol and trading day, in the
layout OUT/<SYMBOL>/<SYMBOL>-YYYY-MM/YYYYMMDD.parquet and the schema in doc/data.md.
Every day the server writes (synthetic_days, recorded_days) and every command line in
scripts/data goes through convert_frame: the same checks and the same layout."""

from __future__ import annotations

import re
from datetime import date, time
from pathlib import Path
from zoneinfo import ZoneInfo

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq

ET = ZoneInfo("America/New_York")
RTH_START = time(9, 30)  # US regular session, inclusive on both ends
RTH_END = time(16, 0)

# The exact schema the backtester reads. Time in nanoseconds, as Databento and the engine keep it:
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
# (every option dropped, backtest "ok" with no trades). Row groups use pyarrow's default size,
# matching the existing data/SPXW files.
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


def write_option_day(df: pl.DataFrame, path) -> None:
    pq.write_table(df.to_arrow().cast(OPTION_ARROW_SCHEMA), path)


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
    """Exact column order / dtypes of the backtester schema.

    Rejects symbols the loader cannot parse.
    """
    bad = df.filter(~pl.col("symbol").str.contains(OCC_BODY.pattern))["symbol"].unique()
    if len(bad):
        raise ConversionError(
            f"{len(bad)} symbols are not OCC 'YYMMDD[C|P]strike*1000', e.g. {bad.head(3).to_list()}"
        )
    return df.select([pl.col(c).cast(t) for c, t in OPTION_SCHEMA.items()])


def split_by_day(df: pl.DataFrame) -> dict[date, pl.DataFrame]:
    df = df.with_columns(pl.col("ts_recv").dt.date().alias("_day"))
    return {
        day: part.drop("_day")
        for (day,), part in df.partition_by("_day", as_dict=True, maintain_order=True).items()
    }


def day_path(out_root: Path, symbol: str, day: date) -> Path:
    return out_root / symbol / f"{symbol}-{day:%Y-%m}" / f"{day:%Y%m%d}.parquet"


def convert_frame(
    df_opt: pl.DataFrame, dfu: pl.DataFrame, out_root: Path, symbol: str, overwrite: bool
) -> list[Path]:
    """Joined, validated, per-day files. Checks every target before writing any."""
    days = split_by_day(conform(attach_underlying(df_opt, dfu)))
    targets = {day: day_path(out_root, symbol, day) for day in days}
    existing = [p for p in targets.values() if p.exists()]
    if existing and not overwrite:
        raise ConversionError(f"{existing[0]} already exists (pass --overwrite to replace)")
    for day, part in days.items():
        targets[day].parent.mkdir(parents=True, exist_ok=True)
        write_option_day(part, targets[day])
    return list(targets.values())
