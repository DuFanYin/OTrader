"""Option CBBO-1m DBN files -> Optra backtest Parquet, one file per trading day.

Each option quote is joined with the underlying quote at the same timestamp
(from es_to_parquet.py), and written to

    OUT/<SYMBOL>/<SYMBOL>-YYYY-MM/YYYYMMDD.parquet

with exactly the schema in doc/data/format.md (through optra.services.days.convert_frame). A DBN file that spans several days
is split by its Eastern-time trading date. Every option timestamp must have an
underlying row; otherwise that file fails and nothing is written for it.

Usage:
  uv run --project app/server --group databento python scripts/data/options_to_parquet.py INPUT [INPUT ...] --underlying es_rth.parquet \\
      --out ../../data --symbol SPXW [--workers 4] [--overwrite]

INPUT is a .dbn or .dbn.zst file, or a directory searched (recursively) for both.

Note on the underlying: SPXW options are SPX index options, but the usual input
here is ES futures (the hedge instrument). ES trades at a basis to SPX and to the
forward of each option expiry; see doc/data/prep.md before relying on IV / Greeks.
"""

from __future__ import annotations

import argparse
import multiprocessing
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import polars as pl

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app" / "server"))  # the optra package
from dbn import read_quotes  # noqa: E402
from optra.services.days import convert_frame, load_underlying_frame  # noqa: E402

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


def option_quotes(quotes: pl.DataFrame, id_to_symbol: dict[int, str]) -> pl.DataFrame:
    """Top-of-book quotes (dbn.read_quotes) with each instrument's symbol (empty for one
    the file does not map), in time order."""
    symbols = pl.DataFrame(
        {"instrument_id": list(id_to_symbol), "symbol": list(id_to_symbol.values())},
        schema={"instrument_id": pl.Int64, "symbol": pl.String},
    )
    return (
        quotes.join(symbols, on="instrument_id", how="left", maintain_order="left")
        .with_columns(pl.col("symbol").fill_null(""))
        .select("symbol", "ts_recv", "bid_px", "ask_px", "bid_sz", "ask_sz")
        .sort("ts_recv", maintain_order=True)
    )


def load_underlying(path: Path) -> pl.DataFrame:
    return load_underlying_frame(pl.read_parquet(path), source=str(path))


def convert_file(
    path: Path, underlying: Path, out_root: Path, symbol: str, overwrite: bool
) -> list[Path]:
    import databento as db

    store = db.DBNStore.from_file(str(path))
    df_opt = option_quotes(read_quotes(store), build_symbol_map(store.metadata.mappings or {}))
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
    ap.add_argument("inputs", nargs="+", help=".dbn / .dbn.zst files or directories")
    ap.add_argument(
        "--underlying", required=True, help="underlying quotes Parquet from es_to_parquet.py"
    )
    ap.add_argument("--out", required=True, help="data root, e.g. Optra's data/")
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
        files.update([*p.rglob("*.dbn"), *p.rglob("*.dbn.zst")] if p.is_dir() else [p])
    if not files:
        print("no .dbn or .dbn.zst files found", file=sys.stderr)
        return 1
    load_underlying(Path(args.underlying))  # fail fast on a bad underlying file

    jobs = [
        (f, Path(args.underlying), Path(args.out), args.symbol, args.overwrite)
        for f in sorted(files)
    ]
    failed = []
    # Spawned, not forked: this process has started polars' thread pool (load_underlying), and a
    # forked worker inherits its locks held, deadlocking at its first polars call.
    spawn = multiprocessing.get_context("spawn")
    with ProcessPoolExecutor(max_workers=args.workers, mp_context=spawn) as pool:
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
