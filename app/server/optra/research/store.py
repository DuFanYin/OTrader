"""The research database, data/experiments.db: the ledger (families, experiments, trials and their days and trades,
strategy versions and their stages, paper days) and the queue of jobs the workers run, in one SQLite file so that a
job's results and its trials change in one transaction."""

from __future__ import annotations

import sqlite3
import threading
from datetime import datetime, timezone
from pathlib import Path


from optra.utils import files

lock = threading.RLock()  # every write; reentrant: a lease checks the code, which writes
_db: sqlite3.Connection | None = None
_db_path: Path | None = None

SCHEMA = """
            CREATE TABLE IF NOT EXISTS families (
                name   TEXT PRIMARY KEY,
                budget INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS experiments (
                id         TEXT PRIMARY KEY,
                created_at TEXT NOT NULL,
                family     TEXT NOT NULL REFERENCES families(name),
                strategy   TEXT NOT NULL,
                spec       TEXT NOT NULL,   -- the request as given, JSON
                days       TEXT NOT NULL,   -- the day files, JSON
                days_hash  TEXT NOT NULL,   -- each day file's path, size and time, hashed
                code       TEXT NOT NULL,   -- code_identity: the engine's core and the strategy's source, hashed
                holdout    INTEGER NOT NULL, -- its days may reach the holdout: the promotion report's
                parent     TEXT,            -- the experiment a robustness check was made from
                note       TEXT NOT NULL DEFAULT ''
            );
            CREATE TABLE IF NOT EXISTS trials (
                id          TEXT PRIMARY KEY,
                family      TEXT NOT NULL REFERENCES families(name),
                strategy    TEXT NOT NULL,
                setting     TEXT NOT NULL,  -- JSON
                costs       TEXT NOT NULL,  -- ledger.Costs, dumped: JSON
                code        TEXT NOT NULL,  -- code_of: the engine's core and the strategy's source, hashed
                status      TEXT NOT NULL,  -- queued, running, done, failed, cancelled
                created_at  TEXT NOT NULL,
                finished_at TEXT,
                summary     TEXT,           -- JSON: the headline numbers
                error       TEXT
            );
            CREATE INDEX IF NOT EXISTS trials_family ON trials(family);
            CREATE INDEX IF NOT EXISTS experiments_created ON experiments(created_at);
            CREATE INDEX IF NOT EXISTS trials_version ON trials(strategy, setting, code);
            CREATE TABLE IF NOT EXISTS strategy_versions (
                id         TEXT PRIMARY KEY,
                strategy   TEXT NOT NULL,
                setting    TEXT NOT NULL,   -- JSON, frozen
                code       TEXT NOT NULL,   -- code_of when registered
                family     TEXT NOT NULL REFERENCES families(name),
                card       TEXT NOT NULL,   -- JSON: the hypothesis, where the edge comes from, when it fails
                stage      TEXT NOT NULL,   -- promotion.STAGES
                created_at TEXT NOT NULL,
                pending       TEXT,         -- a move under way: the stage it goes to (versions.advance)
                pending_note  TEXT,         -- who approved it and why, or why it goes back
                pending_error TEXT          -- why its last step failed (it is tried again)
            );
            CREATE TABLE IF NOT EXISTS strategy_sources (
                hash  TEXT PRIMARY KEY,  -- code_identity.strategy_hash
                files TEXT NOT NULL      -- JSON: each file's name and text
            );
            CREATE TABLE IF NOT EXISTS strategy_installs (
                at        TEXT NOT NULL,
                file      TEXT NOT NULL,  -- strategy/<account>/<file>
                account   TEXT NOT NULL,  -- paper or live
                hash      TEXT NOT NULL,  -- code_identity.strategy_hash: its code
                installed INTEGER NOT NULL, -- 1 installed for the live engine, 0 taken out
                note      TEXT NOT NULL   -- who approved and why, or why it was taken out
            );
            CREATE TABLE IF NOT EXISTS stage_events (
                version_id TEXT NOT NULL REFERENCES strategy_versions(id),
                at         TEXT NOT NULL,
                from_stage TEXT NOT NULL,
                to_stage   TEXT NOT NULL,
                note       TEXT NOT NULL    -- the gates passed, or who approved and why
            );
            CREATE TABLE IF NOT EXISTS paper_days (
                strategy_name  TEXT NOT NULL,  -- the live engine's
                strategy       TEXT NOT NULL,  -- its class
                setting        TEXT NOT NULL,  -- JSON, as the live engine runs it
                day            TEXT NOT NULL,
                paper_net      REAL NOT NULL,  -- the paper fills' cash flow
                backtest_net   REAL NOT NULL,  -- a backtest of the same recorded day's
                paper_fills    INTEGER NOT NULL,
                backtest_fills INTEGER NOT NULL,
                unmatched      INTEGER NOT NULL,  -- fills on one side only
                code           TEXT NOT NULL DEFAULT '{}',  -- JSON: the engine and the strategy that traded
                backtest_slippage REAL,  -- USD the backtest's fills paid over the mids, at the version's costs
                paper_slippage    REAL,  -- the same less the matched paper fills' price differences
                PRIMARY KEY (strategy_name, day)
            );
            CREATE TABLE IF NOT EXISTS experiment_trials (
                experiment_id TEXT NOT NULL REFERENCES experiments(id),
                trial_id      TEXT NOT NULL REFERENCES trials(id),
                position      INTEGER NOT NULL,
                PRIMARY KEY (experiment_id, trial_id)
            );
            CREATE TABLE IF NOT EXISTS jobs (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                kind        TEXT NOT NULL,     -- trials (an experiment's), batch (a run whose result goes back)
                ref         TEXT,              -- trials: the experiment
                name        TEXT,              -- a run that stops the one before of the same name
                priority    INTEGER NOT NULL,  -- the higher first
                payload     TEXT NOT NULL,     -- JSON: trials {"trial_ids"}, batch the batch itself
                status      TEXT NOT NULL,     -- queued, leased, done, failed, cancelled
                worker      TEXT,
                lease_until REAL,
                attempts    INTEGER NOT NULL DEFAULT 0,
                created_at  TEXT NOT NULL,
                finished_at TEXT,
                error       TEXT,
                result      BLOB,              -- batch: the engine's output (runner.read), until taken
                batch_key   TEXT,              -- jobs of a kind with the same key can run as one batch
                lead        INTEGER            -- leased: the job whose batch it runs in (itself, or another's)
            );
            CREATE INDEX IF NOT EXISTS jobs_status ON jobs(status, priority);
            CREATE INDEX IF NOT EXISTS jobs_ref ON jobs(ref);
            CREATE TABLE IF NOT EXISTS trial_results (
                trial_id TEXT PRIMARY KEY REFERENCES trials(id),
                days     TEXT NOT NULL,  -- JSON, a column a field: date (YYYYMMDD), pnl, net_pnl, fees, orders,
                                         -- flat, max_margin, max_delta, max_gamma, max_theta (from 2026-10-17),
                                         -- attribution {delta, gamma, theta, vega, other, fills}
                trades   TEXT NOT NULL   -- JSON, a column a field: batch.trade_columns
            ) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS day_underlying (
                day   TEXT NOT NULL,
                root  TEXT NOT NULL,      -- the days' portfolio (SPXW, SPY)
                open  REAL NOT NULL,      -- the underlying's first and last mid of the day
                close REAL NOT NULL,
                PRIMARY KEY (day, root)
            );
        """


def db_path() -> Path:
    return files.data_root() / "experiments.db"


def db() -> sqlite3.Connection:
    """The connection, opened (and the schema made) on first use and again when data/ moves (the tests)."""
    global _db, _db_path
    path = db_path()
    if _db is None or _db_path != path:
        path.parent.mkdir(parents=True, exist_ok=True)
        _db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        _db.row_factory = sqlite3.Row
        _db.execute("PRAGMA journal_mode=WAL")
        _db.execute("PRAGMA foreign_keys=ON")
        _db.executescript(SCHEMA)
        _db_path = path
    return _db


def now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")
