"""Where the repository and its data are, and the data files listed."""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import List


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
