"""Quotes the live engine recorded (OPTRA_RECORD_DIR) -> backtest Parquet, to replay a paper day.

The live engine's MarketDataEngine writes each portfolio as its quotes stood at the end of each
minute, a CSV a day: <dir>/<PORTFOLIO>/<PORTFOLIO>-YYYY-MM/YYYYMMDD.csv with the columns
ts_ns (the minute's end, ns since the epoch), symbol (YYMMDD, C or P, the strike in thousandths:
the backtest data's name), bid, ask, underlying_bid, underlying_ask. This writes the same days in
the backtest layout and schema (through days.convert_frame): sizes are not recorded and are
written as 0.

Used by POST /api/data/recorded, the daily report and the chains of recorded days; from the command
line, scripts/data/recorded_to_parquet.py.
"""

from __future__ import annotations

from pathlib import Path

import polars as pl

from optra.services.days import ConversionError, convert_frame, load_underlying_frame


def csv_files(inputs: list[Path]) -> list[Path]:
    files: list[Path] = []
    for path in inputs:
        files += sorted(path.rglob("*.csv")) if path.is_dir() else [path]
    return files


def day_frames(path: Path) -> tuple[pl.DataFrame, pl.DataFrame]:
    """A recorded day -> (option quotes, the underlying) as convert_frame takes them."""
    raw = pl.read_csv(path, schema_overrides={"ts_ns": pl.Int64, "symbol": pl.Utf8})
    if raw.is_empty():
        raise ConversionError(f"{path} has no quotes")
    stamped = raw.with_columns(
        pl.from_epoch("ts_ns", time_unit="ns")
        .dt.replace_time_zone("UTC")
        .dt.convert_time_zone("America/New_York")
        .alias("ts_recv")
    )
    options = stamped.select(
        "symbol",
        "ts_recv",
        pl.col("bid").alias("bid_px"),
        pl.col("ask").alias("ask_px"),
        pl.lit(0, pl.Int64).alias("bid_sz"),
        pl.lit(0, pl.Int64).alias("ask_sz"),
    )
    underlying = stamped.group_by("ts_recv", maintain_order=True).agg(
        pl.col("underlying_bid").last().alias("bid_px"),
        pl.col("underlying_ask").last().alias("ask_px"),
    ).with_columns(pl.lit(0, pl.Int64).alias("bid_sz"), pl.lit(0, pl.Int64).alias("ask_sz"))
    return options, load_underlying_frame(underlying, source=str(path))


def convert(inputs: list[Path], out: Path, symbol: str | None = None,
            overwrite: bool = False) -> tuple[list[Path], list[str]]:
    """Every recorded CSV under the inputs as backtest days under out (the portfolio's root from
    each CSV's folder, unless symbol is given): the files written, and the days that failed."""
    written: list[Path] = []
    failed: list[str] = []
    for path in csv_files(inputs):
        try:
            options, underlying = day_frames(path)
            written += convert_frame(options, underlying, out,
                                         symbol or path.parent.parent.name, overwrite)
        except Exception as exc:  # report per day; keep converting the others
            failed.append(f"{path}: {type(exc).__name__}: {exc}")
    return written, failed
