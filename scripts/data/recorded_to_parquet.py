"""Quotes the live engine recorded (OPTRA_RECORD_DIR) -> backtest Parquet, to replay a paper day: the
command line of optra.services.recorded_days (the server does the same: POST /api/data/recorded,
and the daily report).

Usage (from the repository root):
  uv run --project app/server python scripts/data/recorded_to_parquet.py data/recorded/SPY \\
      --out data/recorded-parquet [--symbol SPY] [--overwrite]
  -> OUT/SPY/SPY-YYYY-MM/YYYYMMDD.parquet, one per CSV found under the input folders or files
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app" / "server"))  # the optra package
from optra.services.recorded_days import convert  # noqa: E402

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("inputs", nargs="+", type=Path, help="recorded CSVs or folders of them")
    ap.add_argument("--out", type=Path, required=True, help="the data root (OUT/SYMBOL/...)")
    ap.add_argument("--symbol", help="the portfolio's root (default: each CSV's folder name)")
    ap.add_argument("--overwrite", action="store_true", help="replace days already written")
    args = ap.parse_args(argv)

    written, failed = convert(args.inputs, args.out, args.symbol, args.overwrite)
    for path in written:
        print(path)
    for line in failed:
        print(line, file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
