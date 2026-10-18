"""Quotes recorded from the live engine as backtest days, to replay a paper day.

scripts/data/record_quotes.py asks the engine for each portfolio's quotes just after every minute and adds them here:
<root>/<PORTFOLIO>/<PORTFOLIO>-YYYY-MM/YYYYMMDD.parquet (the New York date), in the backtest schema, each quote
stamped with the end of its minute; sizes are not recorded and are written as 0. The day so far is written whole again
each minute (write_option_day: read back before it replaces the file, so a reader sees the day as of a minute), and a
recorder started again reads the day from its file and goes on from the last minute in it.
"""

from __future__ import annotations

from datetime import date
from pathlib import Path

import polars as pl

from optra.data.days import ET, conform, day_path, read_option_day, write_option_day


def minute_rows(end_ns: int, quotes: list[tuple[str, float, float]], underlying_bid: float,
                underlying_ask: float) -> pl.DataFrame:
    """A minute's quotes, (symbol, bid, ask) in the backtest data's names, as day rows stamped end_ns."""
    n = len(quotes)
    return conform(pl.DataFrame({
        "symbol": [q[0] for q in quotes],
        "ts_recv": pl.from_epoch(pl.Series([end_ns] * n, dtype=pl.Int64), time_unit="ns")
        .dt.replace_time_zone("UTC").dt.convert_time_zone(ET.key),
        "bid_px": [float(q[1]) for q in quotes],
        "ask_px": [float(q[2]) for q in quotes],
        "bid_sz": [0] * n,
        "ask_sz": [0] * n,
        "underlying_bid_px": [float(underlying_bid)] * n,
        "underlying_ask_px": [float(underlying_ask)] * n,
        "underlying_bid_sz": [0] * n,
        "underlying_ask_sz": [0] * n,
    }))


def add_minute(held: dict[str, tuple[date, pl.DataFrame]], root: Path, portfolio: str,
               minute: pl.DataFrame) -> Path | None:
    """The minute (minute_rows) added to the portfolio's day and the day written; held keeps each portfolio's day
    between minutes (empty after a start: the day is read from its file). None: the day has that minute already."""
    end = minute["ts_recv"][0]
    day = end.astimezone(ET).date()
    if portfolio not in held or held[portfolio][0] != day:
        path = day_path(root, portfolio, day)
        held[portfolio] = (day, conform(read_option_day(path)) if path.exists() else minute.clear())
    rows = held[portfolio][1]
    if not rows.is_empty() and rows["ts_recv"].max() >= end:
        return None
    rows = pl.concat([rows, minute])
    path = day_path(root, portfolio, day)
    path.parent.mkdir(parents=True, exist_ok=True)
    write_option_day(rows, path)
    held[portfolio] = (day, rows)
    return path
