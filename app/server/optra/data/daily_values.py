"""Values by day the system works out from the days themselves (data/daily/<series>.csv, the values by day strategies
read as of the day before; doc/data.md): <PORTFOLIO>_minute_rv, the realized vol of the underlying's minutes, %
(the root of the summed squared log returns of its mid, minute to minute), for a day of at least 200 minutes.

    PYTHONPATH=app/server uv run --project app/server python -m optra.data.daily_values data/SPY
    ... optra.data.daily_values data/recorded/SPY --days 2026-10-19      (in quant-system/)

The paper engine's days are added by the end-of-day report (services.session) from the day recorded.
"""

from __future__ import annotations

import argparse
import math
from datetime import date
from pathlib import Path

import polars as pl
import pyarrow.parquet as pq

from optra.data.days import as_doubles, day_of
from optra.utils import files

MIN_RETURNS = 200  # minute returns a day needs (a half day has 210)


def daily_dir() -> Path:
    return files.data_root() / "daily"


def minute_rv(path: Path | str) -> float | None:
    """The day's realized vol of the underlying's mid, minute to minute, %; None for a day of fewer minutes."""
    # The underlying's quote repeats on every option's row of its minute: each once, then as read_option_day reads.
    rows = pq.read_table(path, columns=["ts_recv", "underlying_bid_px", "underlying_ask_px"])
    df = as_doubles(rows.group_by(rows.column_names, use_threads=False).aggregate([]))
    mids = (df.with_columns(((pl.col("underlying_bid_px") + pl.col("underlying_ask_px")) / 2).alias("mid"))
            .filter(pl.col("mid") > 0)
            .sort("ts_recv")
            .group_by(pl.col("ts_recv").dt.truncate("1m"), maintain_order=True).agg(pl.col("mid").first())
            .sort("ts_recv")["mid"].to_numpy())
    if len(mids) - 1 < MIN_RETURNS:
        return None
    returns = [math.log(b / a) for a, b in zip(mids[:-1], mids[1:])]
    return math.sqrt(sum(r * r for r in returns)) * 100.0


def read_series(path: Path) -> dict[str, float]:
    if not path.is_file():
        return {}
    out = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith("#") and "," in line:
            day, value = line.split(",", 1)
            try:
                out[day] = float(value)
            except ValueError:
                continue  # a header line
    return out


def add_minute_rv(days: list[Path], portfolio: str, out: Path | None = None) -> Path:
    """Each day's minute_rv written into <out>/<portfolio>_minute_rv.csv (default data/daily), a day already there
    replaced; days of too few minutes left out."""
    out = out or daily_dir()
    out.mkdir(parents=True, exist_ok=True)
    path = out / f"{portfolio}_minute_rv.csv"
    series = read_series(path)
    for day_file in days:
        day = day_of(day_file)
        value = minute_rv(day_file) if day else None
        if value is not None:
            series[str(day)] = value
    path.write_text("".join(f"{d},{v:.10g}\n" for d, v in sorted(series.items())))
    return path


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", help="a portfolio's days, data/<PORTFOLIO> (its name is the series')")
    ap.add_argument("--days", nargs="*", help="only these days, YYYY-MM-DD (default every day in the folder)")
    ap.add_argument("--out", help="the folder of the values by day (default data/daily)")
    args = ap.parse_args(argv)
    folder = Path(args.folder)
    wanted = {date.fromisoformat(d) for d in args.days} if args.days else None
    days = sorted(p for p in folder.glob("*/*.parquet") if wanted is None or day_of(p) in wanted)
    path = add_minute_rv(days, folder.name, Path(args.out) if args.out else None)
    print(f"{len(days)} days -> {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
