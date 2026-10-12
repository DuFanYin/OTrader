"""Reading Databento DBN quote records (BBO / CBBO) into regular-session top-of-book frames, for
es_to_parquet.py and options_to_parquet.py (the optra package must be importable: they put it on
the path)."""

from __future__ import annotations

from datetime import date

import numpy as np
import polars as pl

from optra.services.days import RTH_END, RTH_START, TS_DTYPE

UNDEF_PRICE = 2**63 - 1  # Databento's "no price" sentinel (fixed-point int64 max)
UNDEF_TIMESTAMP = 2**64 - 1  # Databento's "no time" sentinel
PRICE_SCALE = 1e9  # DBN prices are fixed-point, 1e-9 units
RECORDS_PER_CHUNK = 1_000_000  # a DBN file is read this many records at a time: bounds the memory

# A DBN file's top of book (top_of_book_frame).
QUOTE_SCHEMA: dict[str, pl.DataType] = {
    "ts_recv": TS_DTYPE,
    "instrument_id": pl.Int64,
    "bid_px": pl.Float64,
    "ask_px": pl.Float64,
    "bid_sz": pl.Int64,
    "ask_sz": pl.Int64,
}


def top_of_book_frame(records: np.ndarray) -> pl.DataFrame:
    """BBO / CBBO records, as DBNStore.to_ndarray gives them, one row each: ts_recv (New York, in
    ns), instrument_id, and the first book level's bid_px and ask_px (null where there is no
    price), bid_sz and ask_sz. Records with no receive time are left out."""
    records = records[records["ts_recv"] != UNDEF_TIMESTAMP]

    def price(name: str) -> pl.Expr:
        return (
            pl.when(pl.col(name) == UNDEF_PRICE)
            .then(None)
            .otherwise(pl.col(name) / PRICE_SCALE)
            .alias(name)
        )

    return pl.DataFrame(
        {
            "ts_recv": records["ts_recv"].astype(np.int64),
            "instrument_id": records["instrument_id"].astype(np.int64),
            "bid_px": records["bid_px_00"].astype(np.int64),
            "ask_px": records["ask_px_00"].astype(np.int64),
            "bid_sz": records["bid_sz_00"].astype(np.int64),
            "ask_sz": records["ask_sz_00"].astype(np.int64),
        }
    ).with_columns(
        pl.col("ts_recv").cast(pl.Datetime("ns", "UTC")).dt.convert_time_zone("America/New_York"),
        price("bid_px"),
        price("ask_px"),
    )


def regular_session(df: pl.DataFrame, start: date | None = None, end: date | None = None) -> pl.DataFrame:
    """The rows quoted 09:30-16:00 New York, both ends included, on days from start to end."""
    keep = pl.col("ts_recv").dt.time().is_between(RTH_START, RTH_END, closed="both")
    if start is not None:
        keep &= pl.col("ts_recv").dt.date() >= start
    if end is not None:
        keep &= pl.col("ts_recv").dt.date() <= end
    return df.filter(keep)


def read_quotes(store, start: date | None = None, end: date | None = None) -> pl.DataFrame:
    """A DBN store's regular-session top of book, in file order, read RECORDS_PER_CHUNK records
    at a time (.dbn or .dbn.zst alike)."""
    frames = [
        regular_session(top_of_book_frame(chunk), start, end)
        for chunk in store.to_ndarray(count=RECORDS_PER_CHUNK)
    ]
    return pl.concat(frames) if frames else pl.DataFrame(schema=QUOTE_SCHEMA)


def parse_day(s: str | None) -> date | None:
    return date.fromisoformat(s) if s else None
