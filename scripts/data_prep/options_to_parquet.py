#!/usr/bin/env python3
"""Option CBBO-1m DBN files -> OTrader backtest Parquet, one file per trading day.

Each option quote is joined with the underlying quote at the same timestamp
(from es_to_parquet.py), and written to

    OUT/<SYMBOL>/<SYMBOL>-YYYY-MM/YYYYMMDD.parquet

with exactly the schema in data/README.md. A DBN file that spans several days
is split by its Eastern-time trading date. Every option timestamp must have an
underlying row; otherwise that file fails and nothing is written for it.

Usage:
  python options_to_parquet.py INPUT [INPUT ...] --underlying es_rth.parquet \\
      --out ../../data --symbol SPXW [--workers 4] [--overwrite]

INPUT is a .dbn file or a directory searched (recursively) for *.dbn.

Note on the underlying: SPXW options are SPX index options, but the usual input
here is ES futures (the hedge instrument). ES trades at a basis to SPX and to the
forward of each option expiry; see README.md before relying on IV / Greeks.
"""

from __future__ import annotations

import argparse
import re
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import date
from pathlib import Path

import polars as pl

from dbn_common import OPTION_SCHEMA, TS_DTYPE, in_rth, ns_to_et, top_of_book, write_option_day

OCC_BODY = re.compile(
    r"^\d{6}[CP]\d{8}$"
)  # YYMMDD + C/P + strike*1000, as parse_occ_symbol expects


class ConversionError(ValueError):
    """A DBN file cannot be converted (bad symbols, missing underlying rows, ...)."""


def occ_body(raw_symbol: str) -> str:
    """'SPXW  250804C05000000' -> '250804C05000000'.

    The root is not stored: the loader parses expiry / right / strike starting at YYMMDD.
    """
    parts = raw_symbol.split()
    return parts[-1] if parts else raw_symbol


def build_symbol_map(mappings: dict) -> dict[int, str]:
    """DBN metadata.mappings {raw_symbol: [{"symbol": instrument_id, ...}]}
    -> {instrument_id: OCC body}."""
    out: dict[int, str] = {}
    for raw, intervals in mappings.items():
        for iv in intervals if isinstance(intervals, (list, tuple)) else [intervals]:
            iid = iv.get("symbol")
            if iid is not None:
                out[int(iid)] = occ_body(raw)
    return out


def records_to_frame(records, id_to_symbol: dict[int, str]) -> pl.DataFrame:
    rows = []
    for rec in records:
        ts_ns = getattr(rec, "ts_recv", None)
        if ts_ns is None:
            continue
        dt = ns_to_et(ts_ns)
        if not in_rth(dt):
            continue
        iid = getattr(rec, "instrument_id", None)
        bid_px, ask_px, bid_sz, ask_sz = top_of_book(rec)
        rows.append(
            {
                "symbol": id_to_symbol.get(int(iid), "") if iid is not None else "",
                "ts_recv": dt,
                "bid_px": bid_px,
                "ask_px": ask_px,
                "bid_sz": bid_sz,
                "ask_sz": ask_sz,
            }
        )
    df = pl.DataFrame(
        rows,
        schema={
            "symbol": pl.String,
            "ts_recv": pl.Datetime("us", "UTC"),
            "bid_px": pl.Float64,
            "ask_px": pl.Float64,
            "bid_sz": pl.Int64,
            "ask_sz": pl.Int64,
        },
        orient="row",
    )
    return df.with_columns(
        pl.col("ts_recv").dt.convert_time_zone("America/New_York").cast(TS_DTYPE)
    ).sort("ts_recv")


def load_underlying(path: Path) -> pl.DataFrame:
    return load_underlying_frame(pl.read_parquet(path), source=str(path))


def load_underlying_frame(dfu: pl.DataFrame, source: str = "underlying frame") -> pl.DataFrame:
    """es_to_parquet output -> ts_recv + underlying_{bid,ask}_{px,sz}, one row per timestamp."""
    missing = {"ts_recv", "bid_px", "ask_px", "bid_sz", "ask_sz"} - set(dfu.columns)
    if missing:
        raise ConversionError(f"{source} is missing columns {sorted(missing)}")
    return (
        dfu.select("ts_recv", "bid_px", "ask_px", "bid_sz", "ask_sz")
        .with_columns(pl.col("ts_recv").cast(TS_DTYPE))
        .unique(subset=["ts_recv"], keep="first")
        .sort("ts_recv")
        .rename({c: f"underlying_{c}" for c in ("bid_px", "ask_px", "bid_sz", "ask_sz")})
    )


def attach_underlying(df_opt: pl.DataFrame, dfu: pl.DataFrame) -> pl.DataFrame:
    missing = (
        df_opt.select("ts_recv").unique().join(dfu.select("ts_recv"), on="ts_recv", how="anti")
    )
    if len(missing):
        sample = ", ".join(str(t) for t in missing["ts_recv"].sort().head(5).to_list())
        raise ConversionError(
            f"{len(missing):,} option timestamps have no underlying quote (e.g. {sample})"
        )
    return df_opt.join(dfu, on="ts_recv", how="left")


def conform(df: pl.DataFrame) -> pl.DataFrame:
    """Exact column order / dtypes of the backtester schema.

    Rejects symbols the loader cannot parse.
    """
    bad = df.filter(~pl.col("symbol").str.contains(OCC_BODY.pattern))["symbol"].unique()
    if len(bad):
        raise ConversionError(
            f"{len(bad)} symbols are not OCC 'YYMMDD[C|P]strike*1000', e.g. {bad.head(3).to_list()}"
        )
    return df.select([pl.col(c).cast(t) for c, t in OPTION_SCHEMA.items()])


def split_by_day(df: pl.DataFrame) -> dict[date, pl.DataFrame]:
    df = df.with_columns(pl.col("ts_recv").dt.date().alias("_day"))
    return {
        day: part.drop("_day")
        for (day,), part in df.partition_by("_day", as_dict=True, maintain_order=True).items()
    }


def day_path(out_root: Path, symbol: str, day: date) -> Path:
    return out_root / symbol / f"{symbol}-{day:%Y-%m}" / f"{day:%Y%m%d}.parquet"


def convert_frame(
    df_opt: pl.DataFrame, dfu: pl.DataFrame, out_root: Path, symbol: str, overwrite: bool
) -> list[Path]:
    """Joined, validated, per-day files. Checks every target before writing any."""
    days = split_by_day(conform(attach_underlying(df_opt, dfu)))
    targets = {day: day_path(out_root, symbol, day) for day in days}
    existing = [p for p in targets.values() if p.exists()]
    if existing and not overwrite:
        raise ConversionError(f"{existing[0]} already exists (pass --overwrite to replace)")
    for day, part in days.items():
        targets[day].parent.mkdir(parents=True, exist_ok=True)
        write_option_day(part, targets[day])
    return list(targets.values())


def convert_file(
    path: Path, underlying: Path, out_root: Path, symbol: str, overwrite: bool
) -> list[Path]:
    import databento as db

    store = db.DBNStore.from_file(str(path))
    df_opt = records_to_frame(store, build_symbol_map(store.metadata.mappings or {}))
    if df_opt.is_empty():
        return []
    return convert_frame(df_opt, load_underlying(underlying), out_root, symbol, overwrite)


def _worker(args: tuple[Path, Path, Path, str, bool]) -> tuple[Path, list[Path], str | None]:
    path = args[0]
    try:
        return path, convert_file(*args), None
    except Exception as exc:  # report per file; keep converting the others
        return path, [], f"{type(exc).__name__}: {exc}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("inputs", nargs="+", help=".dbn files or directories")
    ap.add_argument(
        "--underlying", required=True, help="underlying quotes Parquet from es_to_parquet.py"
    )
    ap.add_argument("--out", required=True, help="data root, e.g. OTrader's data/")
    ap.add_argument(
        "--symbol",
        default="SPXW",
        help="portfolio / underlying name used for the folder (default SPXW)",
    )
    ap.add_argument(
        "--workers", type=int, default=None, help="parallel processes (default: CPU count)"
    )
    ap.add_argument("--overwrite", action="store_true", help="replace existing day files")
    args = ap.parse_args(argv)

    files: set[Path] = set()
    for item in args.inputs:
        p = Path(item)
        if not p.exists():
            print(f"not found: {p}", file=sys.stderr)
            return 1
        files.update(p.rglob("*.dbn") if p.is_dir() else [p])
    if not files:
        print("no .dbn files found", file=sys.stderr)
        return 1
    load_underlying(Path(args.underlying))  # fail fast on a bad underlying file

    jobs = [
        (f, Path(args.underlying), Path(args.out), args.symbol, args.overwrite)
        for f in sorted(files)
    ]
    failed = []
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        for i, fut in enumerate(as_completed([pool.submit(_worker, j) for j in jobs]), 1):
            path, written, err = fut.result()
            if err:
                failed.append(path.name)
                print(f"[{i}/{len(jobs)}] FAILED {path.name}: {err}", file=sys.stderr)
            else:
                names = ", ".join(p.name for p in written) or "no regular-session rows"
                print(f"[{i}/{len(jobs)}] {path.name} -> {names}")
    if failed:
        print(f"{len(failed)} of {len(jobs)} files failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
