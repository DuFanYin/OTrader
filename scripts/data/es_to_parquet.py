"""ES continuous BBO-1m DBN files -> one regular-session Parquet of underlying quotes.

The result is the `--underlying` input of options_to_parquet.py. Rows outside
09:30-16:00 ET are dropped; duplicate timestamps (overlapping downloads) keep the
first file's row, in sorted file-name order.

Usage:
  uv run --project app/server --group databento python scripts/data/es_to_parquet.py INPUT [INPUT ...] -o es_continuous_bbo_1m_rth.parquet \\
      [--start 2025-03-01] [--end 2025-08-31]

INPUT is a .dbn or .dbn.zst file, or a directory searched (recursively) for both.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import databento as db
import polars as pl

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app" / "server"))  # the optra package
from dbn import parse_day, read_quotes  # noqa: E402

def collect_inputs(inputs: list[str]) -> list[Path]:
    files: set[Path] = set()
    for item in inputs:
        p = Path(item)
        if p.is_dir():
            files.update([*p.rglob("*.dbn"), *p.rglob("*.dbn.zst")])
        elif p.is_file():
            files.add(p)
        else:
            raise FileNotFoundError(item)
    return sorted(files)


def priced(quotes: pl.DataFrame) -> pl.DataFrame:
    """The quotes with a bid or an ask."""
    return quotes.filter(pl.col("bid_px").is_not_null() | pl.col("ask_px").is_not_null())


def underlying_quotes(frames: list[pl.DataFrame]) -> pl.DataFrame:
    """The files' quotes, one row per timestamp (a timestamp in several files keeps the first
    file's, files in name order), in time order."""
    return (
        pl.concat(frames)
        .unique(subset=["ts_recv"], keep="first", maintain_order=True)
        .sort("ts_recv", maintain_order=True)
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("inputs", nargs="+", help=".dbn / .dbn.zst files or directories")
    ap.add_argument("-o", "--output", required=True, help="output Parquet path")
    ap.add_argument("--start", help="first trading day to keep (YYYY-MM-DD, ET)")
    ap.add_argument("--end", help="last trading day to keep (YYYY-MM-DD, ET)")
    args = ap.parse_args(argv)

    files = collect_inputs(args.inputs)
    if not files:
        print("no .dbn or .dbn.zst files found", file=sys.stderr)
        return 1
    start, end = parse_day(args.start), parse_day(args.end)
    frames: list[pl.DataFrame] = []
    for path in files:
        frames.append(priced(read_quotes(db.DBNStore.from_file(str(path)), start, end)))
        print(f"  {path.name}: +{len(frames[-1]):,} regular-session rows")
    rows = sum(len(f) for f in frames)
    if rows == 0:
        print("no regular-session rows in range", file=sys.stderr)
        return 1
    df = underlying_quotes(frames)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    df.write_parquet(out)
    print(f"wrote {len(df):,} rows ({rows - len(df):,} duplicate timestamps dropped) -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
