"""Which code a result comes from, so a result is reused exactly as long as that code is unchanged. The build says it
(engine/cmake/code_identity.cmake): beside each program and strategy library, <file>.code holds the hash of every
file of the repository the compiler read for it. Nothing here lists sources or compares times.

- entry_backtest.code: "core", all a backtest is built from, so a change to any of it can change any result;
  "engine", the trading core every program shares (what a trading engine reports in GetStatus: a version's gate to
  paper checks the engine it would trade on is the one it was tested on);
- <library>.so.code: "strategy", the strategy's own files under strategy/ by name and content, so a version installed
  for an account is the same code as the research file it came from; and "files", each with its SHA-256, so a
  library whose files changed since it was built is refused until it is built again (check_build).
"""

from __future__ import annotations

import hashlib
import json
from functools import cache
from pathlib import Path
from typing import Any

from optra.utils import files


def engine_dir() -> Path:
    return files.repo_root() / "engine"


def entry_backtest() -> Path:
    return engine_dir() / "build" / "entry_backtest"


@cache
def _read(path: str, stamp: tuple[int, int]) -> dict[str, Any]:
    return json.loads(Path(path).read_text())


def built_code(built: Path) -> dict[str, Any]:
    """What a program or a strategy library was built from (<file>.code beside it); ValueError when it was not built
    or its build wrote none."""
    record = Path(f"{built}.code")
    if not built.exists() or not record.exists():
        raise ValueError(f"{built.name} is not built (or was built before builds said what from): ./build.sh r at "
                         "the repository root, or build_strategies")
    st = record.stat()
    return _read(str(record), (st.st_size, st.st_mtime_ns))


def core_hash() -> str:
    return built_code(entry_backtest())["core"]


def engine_hash() -> str:
    return built_code(entry_backtest())["engine"]


def strategy_hash(library: Path) -> str:
    """A strategy library's code: its files under strategy/ by name and content."""
    return built_code(library)["strategy"]


def strategy_files(library: Path) -> list[Path]:
    """The files under strategy/ a library was built from."""
    return [files.repo_root() / f["path"] for f in built_code(library)["files"]]


def strategy_sources(library: Path) -> dict[str, str]:
    """A strategy's files, by name: what the ledger keeps of the code a result came from."""
    return {p.name: p.read_text() for p in strategy_files(library)}


def changed_since_built(library: Path) -> list[str]:
    """The files of a strategy library that are not as it was built from them (changed or gone)."""
    root = files.repo_root()
    return [f["path"] for f in built_code(library)["files"]
            if not (root / f["path"]).is_file()
            or hashlib.sha256((root / f["path"]).read_bytes()).hexdigest() != f["sha256"]]


def check_build(strategy: dict[str, Any] | None = None) -> None:
    """Refuses (ValueError) an entry_backtest whose build did not say what it was built from, and, given a strategy
    class as --list-strategies lists it, a library of it whose files changed since it was built: results would be
    credited to code they did not come from."""
    built_code(entry_backtest())
    if strategy and strategy.get("library"):
        if changed := changed_since_built(Path(strategy["library"])):
            raise ValueError(f"{Path(strategy['library']).name} was built before {', '.join(changed)} changed: build "
                             "it again (put_strategy, or build_strategies) before running research on it")
