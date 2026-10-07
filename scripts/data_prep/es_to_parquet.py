#!/usr/bin/env python3
"""ES continuous BBO-1m DBN files -> one regular-session Parquet of underlying quotes.

The result is the `--underlying` input of options_to_parquet.py. Rows outside
09:30-16:00 ET are dropped; duplicate timestamps (overlapping downloads) keep the
first file's row, in sorted file-name order.

Usage:
  python es_to_parquet.py INPUT [INPUT ...] -o es_continuous_bbo_1m_rth.parquet \\
      [--start 2025-03-01] [--end 2025-08-31]

INPUT is a .dbn file or a directory searched (recursively) for *.dbn.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import databento as db
import polars as pl

from dbn_common import TS_DTYPE, in_rth, ns_to_et, parse_day, top_of_book


def collect_inputs(inputs: list[str]) -> list[Path]:
    files: set[Path] = set()
    for item in inputs:
        p = Path(item)
        if p.is_dir():
            files.update(p.rglob("*.dbn"))
        elif p.is_file():
            files.add(p)
        else:
            raise FileNotFoundError(item)
    return sorted(files)


def records_to_rows(records, start=None, end=None) -> list[dict]:
    rows = []
    for rec in records:
        ts_ns = getattr(rec, "ts_recv", None) or getattr(rec, "ts_event", None)
        if ts_ns is None:
            continue
        dt = ns_to_et(ts_ns)
        if not in_rth(dt) or (start and dt.date() < start) or (end and dt.date() > end):
            continue
        bid_px, ask_px, bid_sz, ask_sz = top_of_book(rec)
        if bid_px is None and ask_px is None:
            continue
        rows.append(
            {
                "ts_recv": dt,
                "instrument_id": getattr(rec, "instrument_id", None),
                "bid_px": bid_px,
                "ask_px": ask_px,
                "bid_sz": bid_sz,
                "ask_sz": ask_sz,
            }
        )
    return rows


def to_frame(rows: list[dict]) -> pl.DataFrame:
    df = pl.DataFrame(
        rows,
        schema={
            "ts_recv": pl.Datetime("us", "UTC"),
            "instrument_id": pl.Int64,
            "bid_px": pl.Float64,
            "ask_px": pl.Float64,
            "bid_sz": pl.Int64,
            "ask_sz": pl.Int64,
        },
        orient="row",
    )
    df = df.with_columns(pl.col("ts_recv").dt.convert_time_zone("America/New_York").cast(TS_DTYPE))
    return df.unique(subset=["ts_recv"], keep="first", maintain_order=True).sort("ts_recv")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("inputs", nargs="+", help=".dbn files or directories")
    ap.add_argument("-o", "--output", required=True, help="output Parquet path")
    ap.add_argument("--start", help="first trading day to keep (YYYY-MM-DD, ET)")
    ap.add_argument("--end", help="last trading day to keep (YYYY-MM-DD, ET)")
    args = ap.parse_args(argv)

    files = collect_inputs(args.inputs)
    if not files:
        print("no .dbn files found", file=sys.stderr)
        return 1
    start, end = parse_day(args.start), parse_day(args.end)
    rows: list[dict] = []
    for path in files:
        n = len(rows)
        rows.extend(records_to_rows(db.DBNStore.from_file(str(path)), start, end))
        print(f"  {path.name}: +{len(rows) - n:,} regular-session rows")
    if not rows:
        print("no regular-session rows in range", file=sys.stderr)
        return 1
    df = to_frame(rows)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    df.write_parquet(out)
    print(f"wrote {len(df):,} rows ({len(rows) - len(df):,} duplicate timestamps dropped) -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
