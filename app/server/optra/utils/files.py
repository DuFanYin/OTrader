"""Where the repository and its data are, and the data files listed."""
from __future__ import annotations

import functools
import re
from dataclasses import dataclass
from pathlib import Path
from typing import List


@functools.cache
def repo_root() -> Path:
    """The Optra repository (data/, engine/build/), wherever the server was started from."""
    return Path(__file__).resolve().parents[4]


@functools.lru_cache(maxsize=16)
def _real(root: Path) -> Path:
    """A root folder resolved, once: a day file is checked against it thousands of times a request."""
    return root.resolve()


def data_root() -> Path:
    """data/: the backtest days, the recorded and synthetic days, the daily values, the databases (tests point it
    at a folder of their own)."""
    return repo_root() / "data"


def backtest_root() -> Path:
    """data/backtest/: the backtest days, one folder per underlying."""
    return data_root() / "backtest"


def ledger_name(path: Path) -> str:
    """A day file as the ledger names it: relative to data/backtest/ for a backtest day (SPY/SPY-2025-03/...),
    to data/ for any other (recorded/SPY/...)."""
    f, backtest = path.resolve(), _real(backtest_root())
    return str(f.relative_to(backtest if f.is_relative_to(backtest) else _real(data_root())))


def from_ledger_name(name: str) -> Path:
    """The day file a ledger name is: an underlying's (an upper-case first folder) under data/backtest/."""
    return (backtest_root() if name.split("/", 1)[0].isupper() else data_root()) / name


def inside_data(path: str | Path) -> Path:
    """A path a request named (relative: to the root), resolved; refused unless it is inside data/."""
    data = _real(data_root())
    named = Path(path)
    full = (named if named.is_absolute() else repo_root() / named).resolve()
    if not full.is_relative_to(data):
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

        # A symbol's days under data/backtest/ (data/backtest/SPXW/SPXW-2025-08/20250804.parquet) as one
        # segment; a folder with no day files is none.
        backtest = base / "backtest"
        if dirname == "data" and backtest.is_dir():
            for subdir in sorted(backtest.iterdir(), key=lambda p: p.name.lower()):
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
