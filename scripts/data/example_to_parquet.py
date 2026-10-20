"""An example of bringing your own data: option and underlying quotes in two CSV files -> backtest days.

Optra reads one format only (doc/data.md, "What the backtester reads"). A vendor's data is converted
into it offline; this is the shape of such a conversion, for data that comes as two CSV files:

  options.csv     timestamp,expiration,strike,right,bid,ask,bid_size,ask_size
                  2025-03-03 09:31:00,2025-03-03,580,C,1.25,1.27,40,55
  underlying.csv  timestamp,bid,ask,bid_size,ask_size
                  2025-03-03 09:31:00,580.10,580.12,300,250

Timestamps are New York time; right is C or P. Every option timestamp needs an underlying quote at
the same timestamp. The rows go through days.convert_frame, the one function every source is written
through: it joins the underlying on, checks the format and writes one file per trading day,
OUT/<SYMBOL>/<SYMBOL>-YYYY-MM/YYYYMMDD.parquet. Another vendor's files need only another way of
reading them into the same columns.

  uv run --project app/server python scripts/data/example_to_parquet.py options.csv underlying.csv \\
      --symbol SPY --out data/backtest [--overwrite]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import polars as pl

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app" / "server"))  # the optra package
from optra.data.days import TS_DTYPE, ConversionError, convert_frame, load_underlying_frame  # noqa: E402


def new_york(column: str) -> pl.Expr:
    return pl.col(column).str.to_datetime(time_unit="ns").dt.replace_time_zone("America/New_York").cast(TS_DTYPE)


def read_options(path: Path) -> pl.DataFrame:
    """The option quotes with the symbol the backtester reads: YYMMDD, C or P, the strike times 1000 in
    eight digits ("250303C00580000")."""
    raw = pl.read_csv(path, schema_overrides={"expiration": pl.String, "right": pl.String})
    symbol = pl.concat_str(
        pl.col("expiration").str.to_date().dt.strftime("%y%m%d"),
        pl.col("right").str.to_uppercase(),
        (pl.col("strike") * 1000).round().cast(pl.Int64).cast(pl.String).str.zfill(8),
    )
    return raw.select(
        symbol.alias("symbol"), new_york("timestamp").alias("ts_recv"),
        pl.col("bid").alias("bid_px"), pl.col("ask").alias("ask_px"),
        pl.col("bid_size").alias("bid_sz"), pl.col("ask_size").alias("ask_sz"),
    )


def read_underlying(path: Path) -> pl.DataFrame:
    raw = pl.read_csv(path)
    return load_underlying_frame(raw.select(
        new_york("timestamp").alias("ts_recv"), pl.col("bid").alias("bid_px"), pl.col("ask").alias("ask_px"),
        pl.col("bid_size").alias("bid_sz"), pl.col("ask_size").alias("ask_sz"),
    ), source=str(path))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("options", type=Path)
    ap.add_argument("underlying", type=Path)
    ap.add_argument("--symbol", required=True, help="the portfolio, e.g. SPY: the folder the days go to")
    ap.add_argument("--out", type=Path, default=Path("data/backtest"),
                    help="the backtest days' root (default data/backtest)")
    ap.add_argument("--overwrite", action="store_true", help="replace days already there")
    args = ap.parse_args(argv)
    try:
        written = convert_frame(read_options(args.options), read_underlying(args.underlying), args.out,
                                args.symbol, args.overwrite)
    except ConversionError as e:
        print(e, file=sys.stderr)
        return 1
    for path in written:
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
