"""The one place a backtest process starts: entry_backtest --batch, many trials over the same days, each day read and
priced once for all of them, through the cache of priced days; and the one place its output is read (read). The server
never calls run_trials itself: its runs are jobs (jobs.py) that a worker runs here and hands back as the engine wrote
them; outside the server (the tests, a script) jobs run here in the calling process."""

from __future__ import annotations

import json
import signal
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Iterator

import orjson

from optra import settings
from optra.research import strategies, code_identity
from optra.utils import files

DAY_CACHE_GB = 100  # config.json "day_cache_gb": what the stored days may take on disk
TRIM_SECONDS = 600  # the cache is held to its budget after a run, at most this often


class Cancelled(Exception):
    """A run stopped by cancel() before it finished."""


def day_cache_dir() -> Path:
    """data/cache/days: backtest days as the engine stores them (the batch's cache_dir), each file keyed by what it
    holds (adapters::stored_day), so a change of other code keeps them. Over its budget the files read longest ago go
    first (trim): the engine sets a file's access time when it reads it."""
    root = files.data_root() / "cache" / "days"
    root.mkdir(parents=True, exist_ok=True)
    return root


_trimmed: dict[Path, float] = {}  # a cache -> when it was last held to its budget


def trim(root: Path) -> None:
    """The cache held to its budget (config.json "day_cache_gb"), unless it was in the last TRIM_SECONDS: every file
    of it is looked at, a few tens of thousands."""
    if time.monotonic() - _trimmed.get(root, -TRIM_SECONDS) < TRIM_SECONDS:
        return
    _trimmed[root] = time.monotonic()
    budget = float(settings.config().get("day_cache_gb", DAY_CACHE_GB)) * 2**30
    stored = [(f, f.stat()) for f in root.glob("*/*") if f.suffix in (".day", ".chain")]
    total = sum(st.st_size for _, st in stored)
    for f, st in sorted(stored, key=lambda e: e[1].st_atime):
        if total <= budget:
            break
        f.unlink(missing_ok=True)
        total -= st.st_size


_running: dict[str, subprocess.Popen] = {}  # a run's name -> its process, while it runs
_stopped: set[subprocess.Popen] = set()      # processes cancel() stopped, until their run ends
_lock = threading.Lock()


def daily_dir() -> Path:
    """The values by day strategies read (data/daily/<series>.csv, prepared outside the system; model::DailyTable)."""
    return files.data_root() / "daily"


def run_trials(day_files: list[Path] | list[str], trials: list[dict[str, Any]], risk_free_rate: float = 0.05,
               iv_price_mode: str = "mid", threads: int = 0, days_ahead: int = 3,
               name: str | None = None) -> bytes:
    """Each trial ({"id", "strategy", "setting", "costs" (ledger.Costs, dumped), "fills"?, "minute_metrics"?, "log"?,
    "log_days"?}) over the days, in one entry_backtest --batch: its output as the engine wrote it (read). No
    trials: only the days' contracts and minutes are stored in the cache (a day's chains are priced and stored as
    the trials that run on it ask for them). days_ahead: days loaded ahead, each held whole in memory. With a
    name, the run can be stopped by cancel(name) (raising Cancelled here), and a new run of the same name stops the
    one before. Refused (ValueError) when the engine or a strategy's library is older than its sources
    (code_identity.check_build); a process killed from outside (out of memory) is a ValueError too."""
    listing = strategies.strategy_listing()
    code_identity.check_build()
    for strategy in {t["strategy"] for t in trials} & listing.keys():
        code_identity.check_build(listing[strategy])
    spec = {"files": [str(f) for f in day_files], "risk_free_rate": risk_free_rate, "iv_price_mode": iv_price_mode,
            "cache_dir": str(cache := day_cache_dir()), "threads": threads, "days_ahead": days_ahead,
            "daily_dir": str(daily_dir()), "trials": trials}
    with tempfile.NamedTemporaryFile("w", suffix=".json") as f:
        json.dump(spec, f)
        f.flush()
        if name is not None:
            cancel(name)
        proc = subprocess.Popen(strategies.program("--batch", f.name), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        if name is not None:
            with _lock:
                _running[name] = proc
        try:
            stdout, stderr = proc.communicate(timeout=6 * 3600)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            raise ValueError("entry_backtest --batch ran for more than 6 hours: stopped") from None
        finally:
            with _lock:
                if name is not None and _running.get(name) is proc:
                    del _running[name]
                stopped = proc in _stopped
                _stopped.discard(proc)
    if stopped:
        raise Cancelled(name or "")
    if proc.returncode < 0:
        sig = signal.Signals(-proc.returncode).name
        raise ValueError(f"entry_backtest --batch was killed ({sig})"
                         + (": most often the system or its slice out of memory" if sig == "SIGKILL" else ""))
    trim(cache)
    if proc.returncode != 0:
        try:
            why = orjson.loads(stdout)["error"]
        except (orjson.JSONDecodeError, KeyError, TypeError):
            why = (stderr or stdout).decode(errors="replace").strip()[-500:]
        raise ValueError(f"entry_backtest --batch failed ({proc.returncode}): {why}")
    return stdout


def run_batch(batch: dict[str, Any], threads: int = 0, days_ahead: int = 3, name: str | None = None) -> bytes:
    """A job's batch ({"files", "trials", "risk_free_rate", "iv_price_mode"}) through run_trials: the engine's output
    as it wrote it, or {"status": "error", "error"} for a run refused."""
    try:
        return run_trials(batch["files"], batch["trials"], batch["risk_free_rate"], batch["iv_price_mode"], threads,
                          days_ahead, name=name)
    except ValueError as e:
        return orjson.dumps({"status": "error", "error": str(e)})


def check(out: bytes) -> None:
    """ValueError unless entry_backtest's output says its run went through (its last line), read without the rest."""
    last = orjson.loads(out[out.rstrip(b"\n").rfind(b"\n") + 1:])
    if last.get("status") != "ok":
        raise ValueError(last.get("error") or "the batch failed")


def read(out: bytes) -> tuple[dict[str, list[Any]], Iterator[dict[str, Any]]]:
    """entry_backtest's output (a line a trial, then the last): the days its trials ran, a column a field ("date"
    YYYYMMDD, "file", "bars", "quotes", "underlying_open", "underlying_close"), and each trial's result, read as it is
    taken: {"id", "result" (its strategy, portfolio and run time), "errors", "days" and "round_trips" (a column a field,
    the P&L by source under "attribution"), and "fills", "log", "chart" when the trial asked for them}; what the days
    add up to is ledger.summary_of's. ValueError when the run failed."""
    check(out)
    lines = out.rstrip(b"\n").split(b"\n")
    return orjson.loads(lines.pop())["days"], (orjson.loads(line) for line in lines)


def rows(columns: dict[str, Any]) -> list[dict[str, Any]]:
    """A table of the output (a column a field) as rows, the P&L by source ("attribution") among the fields; the
    values a strategy wrote ("values", by name) as each row's "values", those of the day only."""
    values = columns.get("values") or {}
    flat = {k: v for k, v in columns.items() if k not in ("attribution", "values")} | columns.get("attribution", {})
    out = [dict(zip(flat, row)) for row in zip(*flat.values())]
    if values:
        for i, row in enumerate(out):
            row["values"] = {name: column[i] for name, column in values.items() if column[i] is not None}
    return out


def cancel(name: str) -> bool:
    """Stops the run of that name, if one is running."""
    with _lock:
        proc = _running.pop(name, None)
        if proc is None or proc.poll() is not None:
            return False
        _stopped.add(proc)
    proc.terminate()
    return True
