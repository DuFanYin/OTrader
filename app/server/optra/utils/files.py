"""File listing, parquet inspection, backtest duration, strategy list."""
from __future__ import annotations

import re
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List


def repo_root() -> Path:
    """The Optra repository (data/, engine/build/), wherever the server was started from."""
    return Path(__file__).resolve().parents[4]


def data_root() -> Path:
    """data/: the backtest days, one folder per underlying (tests point it at a folder of their own)."""
    return repo_root() / "data"


def inside_data(path: str | Path) -> Path:
    """A path a request named (relative: to the root), resolved; refused unless it is inside data/."""
    data = data_root().resolve()
    named = Path(path)
    full = (named if named.is_absolute() else repo_root() / named).resolve()
    if full != data and data not in full.parents:
        raise ValueError(f"{path} is not under data/")
    return full


# -----------------------------------------------------------------------------
# File listing
# -----------------------------------------------------------------------------


@dataclass
class FileInfo:
    """File metadata used by `/api/files`."""

    name: str
    path: str
    type: str  # "dbn" | "parquet" | "segment"
    size_bytes: int
    number_of_days: int | None = None
    file_count: int | None = None
    date_start: str | None = None
    date_end: str | None = None


_DAY_STEM_RE = re.compile(r"^\d{8}$")


def _format_day_token(day_token: str) -> str | None:
    if not _DAY_STEM_RE.match(day_token):
        return None
    return f"{day_token[0:4]}-{day_token[4:6]}-{day_token[6:8]}"


def list_files(root: Path | None = None) -> List[FileInfo]:
    """List .dbn and .parquet under download/ and data/."""
    if root is None:
        root = repo_root()

    files: List[FileInfo] = []
    for dirname in ("download", "data"):
        base = root / dirname
        if not base.exists() or not base.is_dir():
            continue
        for entry in sorted(base.iterdir(), key=lambda p: p.name.lower()):
            if not entry.is_file():
                continue
            ext = entry.suffix.lower()
            if ext == ".dbn":
                ftype = "dbn"
            elif ext == ".parquet":
                ftype = "parquet"
            else:
                continue
            rel_path = entry.relative_to(root).as_posix()
            files.append(
                FileInfo(
                    name=entry.name,
                    path=rel_path,
                    type=ftype,
                    size_bytes=entry.stat().st_size,
                )
            )

        # A symbol's days under data/ (data/SPXW/SPXW-2025-08/20250804.parquet) as one segment;
        # a folder with no day files (data/raw, the DBN downloads) is none.
        if dirname == "data":
            for subdir in sorted(base.iterdir(), key=lambda p: p.name.lower()):
                if not subdir.is_dir():
                    continue
                parquet_files = sorted(
                    p for p in subdir.rglob("*.parquet") if _DAY_STEM_RE.match(p.stem)
                )
                if not parquet_files:
                    continue
                total_size = sum(p.stat().st_size for p in parquet_files)
                day_tokens: set[str] = set()
                for p in parquet_files:
                    stem = p.stem
                    if _DAY_STEM_RE.match(stem):
                        day_tokens.add(stem)
                sorted_days = sorted(day_tokens)
                date_start = _format_day_token(sorted_days[0]) if sorted_days else None
                date_end = _format_day_token(sorted_days[-1]) if sorted_days else None
                rel_path = subdir.relative_to(root).as_posix()
                files.append(
                    FileInfo(
                        name=subdir.name,
                        path=rel_path,
                        type="segment",
                        size_bytes=total_size,
                        number_of_days=len(day_tokens) if day_tokens else len(parquet_files),
                        file_count=len(parquet_files),
                        date_start=date_start,
                        date_end=date_end,
                    )
                )
    files.sort(key=lambda f: (f.type, f.path))
    return files


# -----------------------------------------------------------------------------
# Parquet inspection (file_info)
# -----------------------------------------------------------------------------


def inspect_parquet_to_dict(rel_path: str) -> Dict[str, Any]:
    """
    Parquet meta: row_count, ts_start, ts_end, unique_ts_count (ts_recv).
    """
    if Path(rel_path).suffix.lower() != ".parquet":
        return {"status": "error", "error": "Only .parquet files are supported."}

    import pandas as pd

    try:
        pq_full = inside_data(rel_path)
    except ValueError as exc:
        return {"status": "error", "error": str(exc)}

    if not pq_full.is_file():
        return {"status": "error", "error": f"Parquet file not found: {rel_path}"}

    try:
        df = pd.read_parquet(pq_full)
    except Exception as e:
        return {"status": "error", "error": f"Failed to read parquet: {e}"}

    row_count = len(df)
    ts_start: str | None = None
    ts_end: str | None = None
    unique_ts_count: int | None = None

    if "ts_recv" in df.columns:
        non_null = df["ts_recv"].dropna()
        if len(non_null) > 0:
            start_dt = pd.to_datetime(non_null.min())
            end_dt = pd.to_datetime(non_null.max())
            ts_start = start_dt.isoformat() if hasattr(start_dt, "isoformat") else str(start_dt)
            ts_end = end_dt.isoformat() if hasattr(end_dt, "isoformat") else str(end_dt)
            unique_ts_count = int(non_null.nunique())

    return {
        "status": "ok",
        "data": {
            "path": rel_path,
            "row_count": row_count,
            "ts_start": ts_start,
            "ts_end": ts_end,
            "unique_ts_count": unique_ts_count,
        },
    }


# -----------------------------------------------------------------------------
# Backtest duration (by symbol, covered days)
# -----------------------------------------------------------------------------


def _infer_symbol(name: str) -> str:
    """Infer symbol from filename: backtest_<SYMBOL>_* or first segment."""
    stem = name.replace(".parquet", "").replace(".PARQUET", "")
    if stem.lower().startswith("backtest_"):
        parts = stem.split("_")
        if len(parts) >= 2:
            return parts[1]
    if "_" in stem:
        return stem.split("_")[0]
    return stem


def _parse_ts(ts: str | None) -> datetime | None:
    if not ts:
        return None
    try:
        return datetime.fromisoformat(ts.replace("Z", "+00:00"))
    except Exception:
        return None


def _covered_days(ts_start: str | None, ts_end: str | None) -> int | None:
    s, e = _parse_ts(ts_start), _parse_ts(ts_end)
    if s is None or e is None:
        return None
    delta = e.date() - s.date()
    return max(0, delta.days + 1)


def backtest_duration() -> Dict[str, Any]:
    """Scan backtest_*.parquet; ts_start/ts_end, covered_days; group by symbol (by_symbol)."""
    all_files = list_files()
    parquet_files = [f for f in all_files if f.type == "parquet" and f.name.startswith("backtest_")]

    file_entries: List[Dict[str, Any]] = []
    for f in parquet_files:
        info = inspect_parquet_to_dict(f.path)
        ts_start: str | None = None
        ts_end: str | None = None
        row_count: int | None = None
        unique_ts_count: int | None = None
        err: str | None = None

        if info.get("status") == "ok" and "data" in info:
            data = info["data"]
            ts_start = data.get("ts_start")
            ts_end = data.get("ts_end")
            row_count = data.get("row_count")
            unique_ts_count = data.get("unique_ts_count")
        else:
            err = info.get("error", "unknown")

        start_dt = _parse_ts(ts_start)
        end_dt = _parse_ts(ts_end)
        date_start_str = start_dt.strftime("%Y-%m-%d") if start_dt else None
        date_end_str = end_dt.strftime("%Y-%m-%d") if end_dt else None
        covered_days = _covered_days(ts_start, ts_end)

        symbol = _infer_symbol(f.name)
        file_entries.append({
            "path": f.path,
            "name": f.name,
            "symbol": symbol,
            "size_bytes": f.size_bytes,
            "row_count": row_count,
            "unique_ts_count": unique_ts_count,
            "ts_start": ts_start,
            "ts_end": ts_end,
            "date_start": date_start_str,
            "date_end": date_end_str,
            "covered_days": covered_days,
            "error": err,
        })

    by_symbol: Dict[str, List[Dict[str, Any]]] = {}
    for e in file_entries:
        sym = e["symbol"]
        if sym not in by_symbol:
            by_symbol[sym] = []
        by_symbol[sym].append(e)

    for sym in by_symbol:
        by_symbol[sym].sort(key=lambda x: (x.get("date_start") or "", x["name"]))

    return {
        "status": "ok",
        "generated_at": datetime.now(tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "count": len(file_entries),
        "by_symbol": dict(sorted(by_symbol.items())),
        "covered_duration": file_entries,
    }
