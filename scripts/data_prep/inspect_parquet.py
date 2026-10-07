#!/usr/bin/env python3
"""Summarise Parquet files: schema, row / timestamp / day counts, head, and whether
each file matches the OTrader backtest schema (data/README.md).

Usage: python inspect_parquet.py PATH [PATH ...] [--head 5]
PATH is a .parquet file or a directory searched recursively.
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

import polars as pl

from dbn_common import OPTION_SCHEMA


def schema_problems(schema: pl.Schema) -> list[str]:
    probs = []
    for col, dtype in OPTION_SCHEMA.items():
        if col not in schema:
            probs.append(f"missing {col}")
        elif col == "ts_recv":
            if not isinstance(schema[col], pl.Datetime):
                probs.append(f"ts_recv is {schema[col]}, not a timestamp")
        elif schema[col] != dtype:
            probs.append(f"{col} is {schema[col]}, expected {dtype}")
    return probs


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--head", type=int, default=5)
    args = ap.parse_args(argv)

    files: list[Path] = []
    for item in args.paths:
        p = Path(item)
        files.extend(sorted(p.rglob("*.parquet")) if p.is_dir() else [p])
    if not files:
        print("no .parquet files found", file=sys.stderr)
        return 1

    months: Counter[str] = Counter()
    for path in files:
        df = pl.read_parquet(path)
        print(f"== {path}  {df.height:,} rows x {df.width} cols")
        if "ts_recv" in df.columns:
            ts = df["ts_recv"]
            days = ts.dt.date().unique().sort()
            months.update(f"{d:%Y-%m}" for d in days)
            print(f"   ts {ts.min()} .. {ts.max()}  unique={ts.n_unique():,}  days={len(days)}")
        if "symbol" in df.columns:
            print(f"   symbols={df['symbol'].n_unique():,}")
            probs = schema_problems(df.schema)
            print("   OTrader schema: " + ("ok" if not probs else "; ".join(probs)))
        if args.head:
            print(df.head(args.head))
    if len(files) > 1 and months:
        print(
            "\ntrading days per month: " + ", ".join(f"{m} {n}" for m, n in sorted(months.items()))
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
