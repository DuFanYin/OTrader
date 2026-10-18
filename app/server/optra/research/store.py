"""The research database, data/experiments.db: the ledger (families, experiments, trials and their days and trades,
strategy versions and their stages, paper days) and the queue of jobs the workers run, in one SQLite file so that a
job's results and its trials change in one transaction."""

from __future__ import annotations

import sqlite3
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import orjson

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
                pending       TEXT,         -- a move under way: the stage it goes to (promotion.advance)
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
            CREATE TABLE IF NOT EXISTS holdout_uses (
                at       TEXT NOT NULL,
                strategy TEXT NOT NULL,   -- class:setting, as the report names it
                days     TEXT NOT NULL    -- the holdout days, first and last
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
        _migrate(_db)
        _db_path = path
    return _db


def _migrate(con: sqlite3.Connection) -> None:
    """Versions of before a move between stages was written before it ran; paper days of before they said the code
    that traded, or what their fills paid over the mids; the experiments' tasks of before the queue became jobs;
    jobs of before they ran in batches together; trials' days and trades of before they were kept a trial a row."""
    version_columns = {r["name"] for r in con.execute("PRAGMA table_info(strategy_versions)")}
    for column in ("pending", "pending_note", "pending_error"):
        if column not in version_columns:
            con.execute(f"ALTER TABLE strategy_versions ADD COLUMN {column} TEXT")
    paper_columns = {r["name"] for r in con.execute("PRAGMA table_info(paper_days)")}
    if "code" not in paper_columns:
        con.execute("ALTER TABLE paper_days ADD COLUMN code TEXT NOT NULL DEFAULT '{}'")
    for column in ("backtest_slippage", "paper_slippage"):
        if column not in paper_columns:
            con.execute(f"ALTER TABLE paper_days ADD COLUMN {column} REAL")
    job_columns = {r["name"] for r in con.execute("PRAGMA table_info(jobs)")}
    for column, kind in (("batch_key", "TEXT"), ("lead", "INTEGER")):
        if column not in job_columns:
            con.execute(f"ALTER TABLE jobs ADD COLUMN {column} {kind}")
    if con.execute("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'tasks'").fetchone() is not None:
        con.execute("BEGIN IMMEDIATE")
        con.execute("""INSERT INTO jobs (id, kind, ref, priority, payload, status, worker, lease_until, attempts,
                                         created_at, finished_at, error)
                       SELECT id, 'trials', experiment_id, 0, json_object('trial_ids', json(trial_ids)), status,
                              worker, lease_until, attempts, created_at, finished_at, error FROM tasks""")
        con.execute("DROP TABLE tasks")
        con.execute("COMMIT")
    if con.execute("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'trial_days'").fetchone() is not None:
        _trial_rows_to_columns(con)


def _trial_rows_to_columns(con: sqlite3.Connection) -> None:
    """trial_days and trial_trades (a row a trial's day, a row a trade) into trial_results, a trial a row, each value
    as it was; a trade of before the engine costed it has no fees, entry_cost, exit_cost or settled (None)."""
    by_source = ("delta", "gamma", "theta", "vega", "other", "fills")
    con.execute("BEGIN IMMEDIATE")
    days: dict[str, list[sqlite3.Row]] = {}
    for r in con.execute("SELECT * FROM trial_days ORDER BY trial_id, day"):
        days.setdefault(r["trial_id"], []).append(r)
    trades: dict[str, list[sqlite3.Row]] = {}
    for r in con.execute("SELECT * FROM trial_trades ORDER BY trial_id, opened, rowid"):
        trades.setdefault(r["trial_id"], []).append(r)

    def columns(rows: list[sqlite3.Row], names: tuple[str, ...]) -> dict[str, Any]:
        return ({n: [r[n] if n in r.keys() else None for r in rows] for n in names}
                | {"attribution": {k: [r[k] for r in rows] for k in by_source}})
    for trial_id in sorted(days.keys() | trades.keys()):
        d = days.get(trial_id, [])
        day_columns = {"date": [int(r["day"].replace("-", "")) for r in d],
                       **columns(d, ("pnl", "net_pnl", "fees", "orders")), "flat": [bool(r["flat"]) for r in d]}
        trade_columns = columns(trades.get(trial_id, []), ("opened", "days", "net", "risk", "r", "hedge_fills", "fees",
                                                           "entry_cost", "exit_cost", "settled"))
        con.execute("INSERT OR REPLACE INTO trial_results VALUES (?, ?, ?)",
                    (trial_id, orjson.dumps(day_columns).decode(), orjson.dumps(trade_columns).decode()))
    con.execute("DROP TABLE trial_trades")
    con.execute("DROP TABLE trial_days")
    con.execute("COMMIT")


def now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")
