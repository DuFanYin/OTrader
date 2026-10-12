"""Run history: every computation's result envelope, kept in SQLite at $OPTRA_HOME/runs.db by its run_id, so a
result can be looked up, cited, rerun and compared later. How many are kept is RETENTION, or `runs` in the deployment's
config.json (settings.py); the oldest go first."""

from __future__ import annotations

import json
import logging
import sqlite3
import threading
import zlib
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

from optra import settings

log = logging.getLogger(__name__)

RETENTION = {"max_runs": 20_000, "max_days": 90}
PRUNE_EVERY = 200   # saves between prunes (and at the first)
INPUT_KEYS = 8      # input fields a listing shows
HEADLINE_KEYS = ("bs", "price", "net_bs_price", "implied_vol", "heston_price", "fit_quality", "gate_decision",
                 "worst_scenario", "worst_mc_shift", "best_strategy", "pnl_std", "n_returned", "winner_method",
                 "base_price", "log_slope", "value", "formula_residual", "mean", "theta_market_price_of_risk",
                 "worst_pnl", "days")


def _pick(d: dict, keys=None, limit: int | None = None) -> dict:
    """A few scalar fields of a dict: those named in `keys` (in that order) when given, else its first ones."""
    names = keys if keys is not None else d.keys()
    out = {k: d[k] for k in names if k in d and isinstance(d[k], (int, float, str, bool))}
    return dict(list(out.items())[:limit]) if limit else out


class RunStore:
    def __init__(self, path: Path):
        self.path = path
        self.lock = threading.Lock()
        self.saves = 0
        self._db: sqlite3.Connection | None = None

    def db(self) -> sqlite3.Connection:
        if self._db is None:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._db = sqlite3.connect(self.path, check_same_thread=False)
            self._db.execute("PRAGMA journal_mode=WAL")
            self._db.executescript("""
                CREATE TABLE IF NOT EXISTS runs (
                    run_id     TEXT PRIMARY KEY,
                    tool       TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    inputs     TEXT NOT NULL,   -- a few input fields, JSON, for listings
                    headline   TEXT NOT NULL,   -- a few headline numbers, JSON, for listings
                    envelope   BLOB NOT NULL    -- the whole result, zlib-compressed JSON
                );
                CREATE INDEX IF NOT EXISTS runs_created ON runs(created_at);
                CREATE INDEX IF NOT EXISTS runs_tool ON runs(tool, created_at);
            """)
        return self._db

    def save(self, envelope: dict) -> None:
        row = (envelope["run_id"], envelope["tool"], envelope["created_at"],
               json.dumps(_pick(envelope.get("input_params") or {}, limit=INPUT_KEYS)),
               json.dumps(_pick(envelope.get("result_summary") or {}, keys=HEADLINE_KEYS, limit=3)),
               zlib.compress(json.dumps(envelope, separators=(",", ":")).encode(), 6))
        with self.lock:
            self.db().execute("INSERT OR REPLACE INTO runs VALUES (?, ?, ?, ?, ?, ?)", row)
            self.db().commit()
            self.saves += 1
            if self.saves % PRUNE_EVERY == 1:
                self._prune()

    def _prune(self) -> None:
        keep = {**RETENTION, **(settings.config().get("runs") or {})}
        cutoff = (datetime.now(timezone.utc) - timedelta(days=float(keep["max_days"]))).isoformat()
        db = self.db()
        db.execute("DELETE FROM runs WHERE created_at < ?", (cutoff,))
        db.execute("DELETE FROM runs WHERE run_id NOT IN (SELECT run_id FROM runs ORDER BY created_at DESC LIMIT ?)",
                   (int(keep["max_runs"]),))
        db.commit()

    def get(self, run_id: str) -> dict | None:
        with self.lock:
            row = self.db().execute("SELECT envelope FROM runs WHERE run_id = ?", (run_id,)).fetchone()
        return json.loads(zlib.decompress(row[0])) if row else None

    def list(self, tool: str | None = None, limit: int = 20) -> list[dict]:
        sql = "SELECT run_id, tool, created_at, inputs, headline FROM runs"
        args: list[Any] = []
        if tool:
            sql += " WHERE tool = ?"
            args.append(tool)
        sql += " ORDER BY created_at DESC LIMIT ?"
        args.append(limit)
        with self.lock:
            rows = self.db().execute(sql, args).fetchall()
        return [{"run_id": r[0], "tool": r[1], "created_at": r[2], "inputs": json.loads(r[3]), "headline": json.loads(r[4])}
                for r in rows]

    def delete(self, run_id: str) -> bool:
        with self.lock:
            n = self.db().execute("DELETE FROM runs WHERE run_id = ?", (run_id,)).rowcount
            self.db().commit()
        return n > 0


_stores: dict[Path, RunStore] = {}
_stores_lock = threading.Lock()


def store() -> RunStore:
    """The store of the current $OPTRA_HOME."""
    path = settings.home() / "runs.db"
    with _stores_lock:
        if path not in _stores:
            _stores[path] = RunStore(path)
        return _stores[path]


def save_quietly(envelope: dict) -> None:
    """Keeps a run; a failure to keep it never fails the computation."""
    try:
        store().save(envelope)
    except Exception as exc:  # noqa: BLE001 (a full disk, a locked file: the result still goes back)
        log.warning("run %s not kept (%s)", envelope.get("run_id"), exc)
