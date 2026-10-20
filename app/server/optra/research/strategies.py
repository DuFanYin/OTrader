"""The strategies there are, and where their code lives: the repository's strategy/ folder, outside the engine. A
strategy is a C++ file built as a library of its own; a program loads the libraries of its folders, and refuses one
built against other strategy headers.

- strategy/demo: the repository's own (the only folder in git): the strategies its tools, tests and public demo run
  (ComboStrategy, the RandomBuyStrategy control, ...). Every program loads them (engine/build/strategies).
- strategy/research: the owner's and the agents' (not in git), written through put (put_strategy) and built alone
  against the strategy API (engine/build/strategies/research). Backtests and experiments load them; an engine that
  trades never does.
- strategy/paper, strategy/live: installed for the paper account and for the live one (not in git): a version's
  code as the ledger keeps it, when it is promoted (paper; pilot and live), or a strategy a person installs
  (engine/build/strategies/paper, .../live). An engine loads the demo ones and those of the account it trades
  (entry_live --strategies), so an agent's code reaches an account only through a person.

CMake builds all four (./build.sh, ./build.sh s); put and install build one file at once, with the same command.
"""

from __future__ import annotations

import contextlib
import fcntl
import json
import os
import re
import subprocess
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any

from optra.research import code_identity
from optra.utils import files

NAME = re.compile(r"[a-z][a-z0-9_]{0,63}")


def strategy_dir() -> Path:
    """strategy/: every strategy, in its three folders."""
    return files.repo_root() / "strategy"


def demo_dir() -> Path:
    return strategy_dir() / "demo"


def source_dir() -> Path:
    """strategy/research: the owner's and the agents' strategies."""
    return strategy_dir() / "research"


ACCOUNTS = ("paper", "live")


def installed_dir(account: str) -> Path:
    """strategy/paper or strategy/live: the strategies installed for that account."""
    return strategy_dir() / account


def library_dir() -> Path:
    """Where the research strategies are built (entry_backtest loads them)."""
    return library_of(source_dir() / "any.cpp").parent


def installed_library_dir(account: str) -> Path:
    """Where those installed for the account are built (the account's engine loads them)."""
    return library_of(installed_dir(account) / "any.cpp").parent


def program(*args: str) -> list[str]:
    """entry_backtest as research runs it: the demo strategies and the research ones."""
    return [str(code_identity.entry_backtest()), "--strategies", str(library_dir()), *args]


_listed: tuple[tuple, dict[str, Any]] | None = None  # (what it was listed from, the listing)
_listing = threading.Lock()


def _libraries() -> tuple:
    """entry_backtest and every strategy library it loads, each with its size and time: what a listing is of."""
    exe = code_identity.entry_backtest()
    found = []
    for d in (exe.parent / "strategies", library_dir()):
        for f in sorted(d.glob("*.so")) if d.is_dir() else []:
            st = f.stat()
            found.append((str(f), st.st_size, st.st_mtime_ns))
    st = exe.stat() if exe.exists() else None
    return (str(exe), st and (st.st_size, st.st_mtime_ns), tuple(found))


def built_strategies() -> dict[str, Any]:
    """entry_backtest --list-strategies: "strategies", each class with its parameters (name, default, min, max,
    integer), the source file it registered from and the library it was loaded from; "refused", the strategy
    libraries not loaded, each with why. Listed again only when the program or a library changed: it is asked on
    every task leased."""
    global _listed
    with _listing:
        now = _libraries()
        if _listed is None or _listed[0] != now:
            _listed = (now, json.loads(subprocess.run(program("--list-strategies", "--json"), capture_output=True, text=True,
                                                      timeout=60, check=True).stdout))
        return _listed[1]


def strategy_listing() -> dict[str, dict[str, Any]]:
    """Each strategy class by name, as built_strategies has it."""
    return {c["name"]: c for c in built_strategies()["strategies"]}


def strategy_classes() -> dict[str, list[dict[str, Any]]]:
    """Each strategy class with its parameters (name, default, min, max, integer)."""
    return {name: c["parameters"] for name, c in strategy_listing().items()}


def unknown_strategy(strategy: str, built: dict[str, Any]) -> ValueError:
    """The error for a class the build does not have, naming the libraries it could not load."""
    refused = "; ".join(f"{Path(r['library']).name}: {r['why']}" for r in built.get("refused", []))
    return ValueError(f"unknown strategy {strategy}"
                      + (f" (strategy libraries not loaded: {refused})" if refused else ""))


def from_demo(source: str | Path) -> bool:
    """A strategy of the repository's (strategy/demo), not one written for research."""
    return Path(source).resolve().is_relative_to(demo_dir().resolve())


_building_here = threading.Lock()


@contextlib.contextmanager
def _building() -> Iterator[None]:
    """One CMake build at a time in the build folder: this process's threads, and any other process that builds
    there (build.sh takes the same file lock)."""
    lock = code_identity.engine_dir() / "build" / ".build.lock"
    with _building_here:
        lock.parent.mkdir(parents=True, exist_ok=True)
        with lock.open("w") as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            yield


def library_of(source: Path) -> Path:
    """Where a strategy file's library is built: build/strategies for demo, build/strategies/<folder> for the rest."""
    folder = source.parent.name
    base = code_identity.engine_dir() / "build" / "strategies"
    return (base if folder == "demo" else base / folder) / f"{source.stem}.so"


def installed_library(account: str, cpp: str) -> Path:
    """The library of strategy/<account>/<cpp> (installed_library_dir)."""
    return installed_library_dir(account) / f"{Path(cpp).stem}.so"


def _cmake(*args: str, timeout: float) -> subprocess.CompletedProcess:
    return subprocess.run(["cmake", *args], capture_output=True, text=True, timeout=timeout)


def build_library(source: Path) -> list[str]:
    """Builds the library of one strategy file as ./build.sh does, through CMake (its target in engine/build, a new
    file's after CMake has looked at the folders again): linked apart, renamed into place with what it was built from
    beside it (code_identity.built_code). The compiler's error lines; none when it built."""
    engine = code_identity.engine_dir()
    if not (engine / "build" / "CMakeCache.txt").exists():
        raise ValueError("the engine is not built: ./build.sh r")
    target = f"strategy_{source.parent.name}_{source.stem}"
    with _building():
        if not (engine / "build" / "strategy" / "CMakeFiles" / f"{target}.dir").is_dir():
            configured = _cmake("-S", str(engine), "-B", str(engine / "build"), timeout=300)
            if configured.returncode != 0:
                return (configured.stdout + configured.stderr).splitlines()[-30:]
        proc = _cmake("--build", str(engine / "build"), "--target", target, timeout=600)
    if proc.returncode == 0:
        return []
    lines = (proc.stdout + proc.stderr).splitlines()
    return [ln for ln in lines if "error" in ln][:40] or lines[-30:]


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    writing = path.with_name(f".{path.name}.{os.getpid()}")
    writing.write_text(text)
    writing.replace(path)


def put(name: str, source: str) -> dict[str, Any]:
    """A research strategy, written as strategy/research/<name>.cpp (in place of the one before) and built alone.
    {"built": False, "errors"} when it does not compile; else {"built": True, "classes": what its library registered,
    each with its parameters, "refused": why some were not, if any}."""
    if not NAME.fullmatch(name):
        raise ValueError("a strategy's name is lower case letters, digits and _, starting with a letter")
    if (demo_dir() / f"{name}.cpp").exists():
        raise ValueError(f"{name} is a demo strategy (strategy/demo): choose another name")
    path = source_dir() / f"{name}.cpp"
    _write(path, source)
    errors = build_library(path)
    if errors:
        return {"built": False, "errors": errors}
    built = built_strategies()
    library = str(library_of(path))
    return {"built": True,
            "classes": [{"name": c["name"], "parameters": c["parameters"]} for c in built["strategies"]
                        if c["library"] == library],
            "refused": [r["why"] for r in built["refused"] if r["library"] == library]}


def build() -> dict[str, Any]:
    """Every strategy library again, after the engine changed: the demo ones, the research ones and the installed
    versions (CMake, optra_strategies), and none whose source is gone. {"built": bool, "errors": the compiler's
    error lines}."""
    with _building():
        proc = subprocess.run(["cmake", "--build", str(code_identity.engine_dir() / "build"), "--target", "optra_strategies",
                               "-j", str(os.cpu_count() or 4)], capture_output=True, text=True, timeout=900)
        _prune()
    lines = (proc.stdout + proc.stderr).splitlines()
    return {"built": proc.returncode == 0,
            "errors": [] if proc.returncode == 0 else [ln for ln in lines if "error:" in ln][:40] or lines[-30:]}


def _prune() -> None:
    """Libraries whose source is gone (moved or deleted): CMake leaves them, and a program would load them still."""
    folders = {code_identity.engine_dir() / "build" / "strategies": demo_dir(), library_dir(): source_dir(),
               **{installed_library_dir(a): installed_dir(a) for a in ACCOUNTS}}
    for libraries, sources in folders.items():
        for library in libraries.glob("*.so"):
            if not (sources / f"{library.stem}.cpp").exists():
                library.unlink()
                Path(f"{library}.code").unlink(missing_ok=True)


def install(sources: dict[str, str], account: str) -> Path:
    """A strategy's code (its files by name, as the ledger keeps them) written to strategy/<account> and built where
    the account's engine loads it from the next time it lists the strategies (in place of an earlier version of the
    same file; strategies already running keep the code they started with). Refused (ValueError) when it does not
    build."""
    if account not in ACCOUNTS:
        raise ValueError(f"no account {account}: {', '.join(ACCOUNTS)}")
    cpp = [name for name in sources if name.endswith(".cpp")]
    if len(cpp) != 1:
        raise ValueError("a strategy is one .cpp file (and its header)")
    stem = Path(cpp[0]).stem
    for name, text in sources.items():
        if Path(name).stem == stem:  # what it includes of strategy/demo is there already
            _write(installed_dir(account) / name, text)
    errors = build_library(installed_dir(account) / cpp[0])
    if errors:
        raise ValueError("it does not build against the engine as it is now:\n" + "\n".join(errors))
    return installed_library(account, cpp[0])


def uninstall(sources: dict[str, str], account: str) -> None:
    """A strategy out of strategy/<account> and its library: the account's engine loads it no more (one loaded keeps
    running until the engine restarts)."""
    for name in sources:
        (installed_dir(account) / name).unlink(missing_ok=True)
        if name.endswith(".cpp"):
            installed_library(account, name).unlink(missing_ok=True)
            Path(f"{installed_library(account, name)}.code").unlink(missing_ok=True)
