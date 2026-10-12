"""Synthetic option days in the backtest Parquet layout, from a model whose answers are known: the
command line of optra.services.synthetic_days (the model and what each argument plants are
described there; the server makes the same days, POST /api/data/synthetic).

Usage (from the repository root):
  uv run --project app/server python scripts/data/synthetic_to_parquet.py --start 2025-03-03 --end 2025-03-07 \\
      --out data --symbol SPY [--spot 600] [--realized-vol 0.15] [--implied-vol 0.18] [--seed 1]
"""

from __future__ import annotations

import argparse
import sys
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app" / "server"))  # the optra package
from optra.services.days import ConversionError  # noqa: E402
from optra.services.synthetic_days import Model, check, generate  # noqa: E402

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--start", required=True, help="first day, YYYY-MM-DD")
    ap.add_argument("--end", required=True, help="last day, YYYY-MM-DD")
    ap.add_argument("--out", required=True, help="data root, e.g. Optra's data/")
    ap.add_argument("--symbol", default="SPY", help="folder / underlying name (default SPY)")
    ap.add_argument("--overwrite", action="store_true", help="replace existing day files")
    d = Model()
    for name, help_ in (
        ("spot", "underlying price at the first open"),
        ("realized_vol", "vol of the underlying during the session"),
        ("overnight_vol", "vol between sessions (default: --realized-vol)"),
        ("drift", "annual drift of the underlying"),
        ("rate", "risk-free rate: the forward is the spot grown at it, quotes discounted at it"),
        ("implied_vol", "at-the-money implied vol of the quotes"),
        ("skew", "implied vol per standard deviation of moneyness"),
        ("smile", "implied vol per squared standard deviation of moneyness"),
        ("momentum", "last 30 minutes' drift as a multiple of the first 30 minutes' return"),
        ("strike_step", "distance between strikes"),
        ("tick", "price increment of every quote (0: no rounding)"),
        ("spread", "option bid-ask width as a fraction of its price (at least one tick)"),
    ):
        ap.add_argument(
            f"--{name.replace('_', '-')}",
            type=float,
            default=getattr(d, name),
            help=f"{help_} (default {getattr(d, name)})",
        )
    for name, help_ in (
        ("strikes", "strikes either side of the open"),
        ("expiries", "nearest daily expiries listed each day, 0DTE included; 6 reach a week out"),
        ("size", "bid and ask size of every quote"),
        ("seed", "random seed"),
    ):
        ap.add_argument(
            f"--{name}",
            type=int,
            default=getattr(d, name),
            help=f"{help_} (default {getattr(d, name)})",
        )
    args = ap.parse_args(argv)

    start, end = date.fromisoformat(args.start), date.fromisoformat(args.end)
    if end < start:
        print("--end is before --start", file=sys.stderr)
        return 1
    fields = Model.__dataclass_fields__
    model = Model(**{k: v for k, v in vars(args).items() if k in fields})
    try:
        check(model)
        written = generate(model, start, end, Path(args.out), args.symbol, args.overwrite)
    except (ValueError, ConversionError) as exc:
        print(exc, file=sys.stderr)
        return 1
    for path in written:
        print(path)
    print(f"{len(written)} days written", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
