"""Values by day the system works out from the days themselves (data/daily/<series>.csv, the values by day strategies
read as of the day before; doc/data.md):
- <PORTFOLIO>_minute_rv, the realized vol of the underlying's minutes, % (the root of the summed squared log returns of
  its mid, minute to minute), for a day of at least 200 minutes;
- with --surface, the day's volatility surface as SurfaceStrategy (strategy/demo/surface.cpp) measures it 15
  minutes before the close, <PORTFOLIO>_<value> for each value it writes (atm_iv_30d, rr25_30d, ..., spot; IVs as
  fractions), then from the spots <PORTFOLIO>_rv_21d (the realized vol of the last 21 of them, a year of 252) and
  <PORTFOLIO>_iv_rv (atm_iv_30d less rv_21d).

    PYTHONPATH=app/server uv run --project app/server python -m optra.data.daily_values data/backtest/SPY [--surface]
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
SURFACE = "SurfaceStrategy"
RV_DAYS = 21
SURFACE_CHUNK = 250  # days a backtest run


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


def write_series(path: Path, series: dict[str, float]) -> None:
    path.write_text("".join(f"{d},{v:.10g}\n" for d, v in sorted(series.items())))


def add_values(days: dict[str, dict[str, float]], out: Path | None = None) -> int:
    """Each day's values ({day: {series: value}}) added to <out>/<series>.csv (default data/daily), a day already
    there kept as it is; how many days were new."""
    out = out or daily_dir()
    out.mkdir(parents=True, exist_ok=True)
    new = set()
    for name in sorted({n for values in days.values() for n in values}):
        path = out / f"{name}.csv"
        series = read_series(path)
        for day, values in days.items():
            if name in values and day not in series:
                series[day] = values[name]
                new.add(day)
        write_series(path, series)
    return len(new)


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
    write_series(path, series)
    return path


def surface(days: list[Path]) -> dict[str, dict[str, float]]:
    """Each day's values as SurfaceStrategy writes them: {YYYY-MM-DD: {value: ...}}."""
    from optra.research import runner

    trial = {"id": "surface", "strategy": SURFACE, "setting": {}, "costs": {}}
    _, trials = runner.read(runner.run_trials(days, [trial]))
    out: dict[str, dict[str, float]] = {}
    for t in trials:
        if t.get("errors"):
            raise ValueError(f"{SURFACE}: {t['errors']}")
        for row in runner.rows(t["days"]):
            d = str(row["date"])
            out[f"{d[:4]}-{d[4:6]}-{d[6:]}"] = row.get("values", {})
    return out


def realized(spots: dict[str, float]) -> dict[str, float]:
    """The realized vol of each day's last RV_DAYS spot-to-spot log returns, a year of 252 (from the RV_DAYS + 1th)."""
    days = sorted(spots)
    returns = [math.log(spots[b] / spots[a]) for a, b in zip(days[:-1], days[1:])]
    return {days[i + 1]: math.sqrt(sum(r * r for r in returns[i + 1 - RV_DAYS:i + 1]) / RV_DAYS * 252.0)
            for i in range(RV_DAYS - 1, len(returns))}


def add_surface(days: list[Path], portfolio: str, out: Path | None = None) -> list[Path]:
    """Each day's surface written into <out>/<portfolio>_<value>.csv (default data/daily), a day already there
    replaced; then <portfolio>_rv_21d and _iv_rv worked out again from all its spots. The files written."""
    out = out or daily_dir()
    out.mkdir(parents=True, exist_ok=True)
    measured: dict[str, dict[str, float]] = {}
    for i in range(0, len(days), SURFACE_CHUNK):
        measured |= surface(days[i:i + SURFACE_CHUNK])
    written = []
    for name in sorted({n for values in measured.values() for n in values}):
        path = out / f"{portfolio}_{name}.csv"
        series = read_series(path)
        for day, values in measured.items():
            if name in values:
                series[day] = values[name]
        write_series(path, series)
        written.append(path)
    rv = realized(read_series(out / f"{portfolio}_spot.csv"))
    iv = read_series(out / f"{portfolio}_atm_iv_30d.csv")
    for name, series in (("rv_21d", rv), ("iv_rv", {d: iv[d] - v for d, v in rv.items() if d in iv})):
        write_series(path := out / f"{portfolio}_{name}.csv", series)
        written.append(path)
    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", help="a portfolio's days, data/backtest/<PORTFOLIO> (its name is the series')")
    ap.add_argument("--days", nargs="*", help="only these days, YYYY-MM-DD (default every day in the folder)")
    ap.add_argument("--out", help="the folder of the values by day (default data/daily)")
    ap.add_argument("--surface", action="store_true", help="the surface's values (SurfaceStrategy), not minute_rv")
    args = ap.parse_args(argv)
    folder = Path(args.folder)
    wanted = {date.fromisoformat(d) for d in args.days} if args.days else None
    days = sorted(p for p in folder.glob("*/*.parquet") if wanted is None or day_of(p) in wanted)
    out = Path(args.out) if args.out else None
    if args.surface:
        written = add_surface(days, folder.name, out)
        print(f"{len(days)} days -> {len(written)} series, {written[0].parent if written else '-'}")
    else:
        print(f"{len(days)} days -> {add_minute_rv(days, folder.name, out)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
