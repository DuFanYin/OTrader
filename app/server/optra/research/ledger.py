"""The ledger of experiments: many backtests of one idea, registered before they run, run in batches by workers, their
results kept day by day.

An experiment is a strategy class, a space of its parameters and a way through it (a grid, random or Latin
hypercube samples), over days and with costs. It expands into trials: one setting each. A trial's id is the
hash of everything that decides its result (the strategy, its setting, the costs and limits, the pricing,
the days and the code: the engine's core and the strategy's source), so the same trial is never run twice: an
experiment that asks for one already run gets its result.

Every trial belongs to a family (a hypothesis and all its variants) and is counted when it is registered, before it
runs: what was tried is what biases a selection, finished or not. A family has a budget of trials (200 unless set),
checked at registration: when to stop on one idea. What deflates a selection is not the family's count, which a new
name would start again, but every trial the ledger had registered by then, whatever its family (tries_by): only the
control's trials are left out.

Trials run in jobs of the queue (jobs.py, the kind "trials"): a batch of trials over all the experiment's days, which
a worker (research.worker, on this machine or another) leases, runs with `entry_backtest --batch` and reports back.

Days on or after a portfolio's holdout start (config.json: {"holdout": {"SPXW": "2025-07-01"}}) cannot be
in an experiment the agent runs: the system's holdout report of a version (promotion.holdout_report) is the agent's
one way to them, and the person's requests the other (infra/access.py).

The ledger and the results are one SQLite file (store.py): experiments, trials, the experiments' trials, families,
and each trial's days (P&L, fees, P&L by source, flat at the close) and trades (round trips with their risk and R).
"""

from __future__ import annotations

import hashlib
import json
import math
import sqlite3
import time
from datetime import date
from pathlib import Path
from typing import Any, Iterable, Literal

import numpy as np
import orjson
from pydantic import BaseModel, ConfigDict, Field

from optra import settings
from optra.infra import access
from optra.research import batch, jobs, runner, code_identity
from optra.research.store import db, lock as _lock, now
from optra.research.space import canonical, expand, resolved
from optra.research.strategies import built_strategies, strategy_classes, unknown_strategy
from optra.data.days import day_of, held_day
from optra.utils import files

DEFAULT_BUDGET = 200
CONTROL = ("RandomBuyStrategy", {"seed": 1.0})  # what a strategy must beat, run on the same days
CONTROL_FAMILY = "control"
BATCH_SIZE = 128  # trials a job holds; the jobs of the same days run together, up to jobs.BATCH_TRIALS

def digest(value: Any, n: int = 16) -> str:
    return hashlib.sha256(canonical(value).encode()).hexdigest()[:n]


def days_hash(day_files: list[Path]) -> str:
    root = files.data_root().resolve()
    entries = []
    for f in day_files:
        st = f.stat()
        entries.append([str(f.resolve().relative_to(root)), st.st_size, st.st_mtime_ns])
    return digest(entries)


def holdout_from(root: str) -> date | None:
    """The first holdout day of a portfolio's data (config.json "holdout"); for one with none of its own while others
    have one, the earliest of theirs (a folder named after no portfolio does not lead around the holdout); None when
    no portfolio has a holdout."""
    holdouts = settings.config().get("holdout") or {}
    value = holdouts.get(root) or min(holdouts.values(), default=None)
    return date.fromisoformat(value) if value else None


def resolve_days(days: str | list[str], start: date | None = None, end: date | None = None,
                 holdout: bool = False) -> list[Path]:
    """A folder of days or the days themselves (under data/), in date order, between start and end, with
    none in a holdout unless `holdout` (the promotion report's trials)."""
    if isinstance(days, list):
        day_files = [files.inside_data(d) for d in days]
    else:
        day_files = batch.days_of(files.inside_data(days))
    day_files = [f for f in day_files if (start is None or (day_of(f) or start) >= start)
                 and (end is None or (day_of(f) or end) <= end)]
    if not day_files:
        raise ValueError("no backtest days")
    for f in day_files:
        root = f.parent.parent.name
        cut = holdout_from(root)
        d = held_day(f) or day_of(f)  # by the quotes' own times: a file renamed does not move its day
        if not holdout and cut is not None and d is not None and d >= cut:
            raise ValueError(f"{f.name} is in {root}'s holdout (from {cut}): experiments cannot use it; "
                             f"the promotion report (backtest_report) is the way to the holdout")
    return day_files


class Costs(BaseModel):
    """What trading costs a run, the one definition of them: the engine takes them whole (entry_backtest's "costs"),
    every tool and the page as one field, each with its default, and a trial records them."""
    model_config = ConfigDict(extra="forbid", frozen=True)

    fee_rate: float = Field(1.2, ge=0, description=(
        "Fees, USD an option contract (IBKR plus exchange fees on SPX: about 1.2); shares pay IBKR's fixed rate, "
        "0.005 a share, 1 an order."))
    fill_position: float = Field(1.0, ge=0, le=10, description=(
        "Where in its spread an order of one leg fills: its mid plus this many half spreads toward its side (1: buys "
        "at the ask, sells at the bid; below 1 inside the spread, above 1 beyond it)."))
    multi_leg_fill_position: float = Field(1.0, ge=0, le=10, description=(
        "The same for an order of several legs (a combo), each leg in its own spread: complex orders often fill "
        "nearer the mids than the legs' quotes."))
    fill: Literal["quote", "shown_size"] = Field("quote", description=(
        "quote: an order fills whole; shown_size: an option's at most the size shown at its side each minute, the "
        "rest in the minutes after (the stricter test)."))
    delay_bars: int = Field(0, ge=0, le=30, description="Minutes an order waits beyond the next before it can fill.")
    risk: dict[str, float] = Field(default_factory=dict, description=(
        "Risk limits in place of the defaults (all off but the order sizes), as the engine checks every order: e.g. "
        "max_margin (USD of margin the holding may take), max_strategy_daily_loss, max_strategy_delta_usd."))

    @classmethod
    def of(cls, recorded: Costs | dict[str, Any] | None) -> Costs:
        """As a trial recorded them, from before 2026-10-17 too (its slippage_bps, gone since, is dropped)."""
        if isinstance(recorded, Costs):
            return recorded
        return cls.model_validate({k: v for k, v in (recorded or {}).items() if k in cls.model_fields})

    def doubled(self) -> Costs:
        """The robustness check's: fees and the fill positions twice what they were."""
        return self.model_copy(update={"fee_rate": self.fee_rate * 2, "fill_position": self.fill_position * 2,
                                       "multi_leg_fill_position": self.multi_leg_fill_position * 2})

    def strict(self) -> Costs:
        """The robustness check's: fills of at most the size shown, a minute late."""
        return self.model_copy(update={"fill": "shown_size", "delay_bars": 1})

    def at_the_quotes(self) -> Costs | None:
        """The same, every leg at its bid or ask; none when it is so already."""
        if self.fill_position >= 1 and self.multi_leg_fill_position >= 1:
            return None
        return self.model_copy(update={"fill_position": max(self.fill_position, 1.0),
                                       "multi_leg_fill_position": max(self.multi_leg_fill_position, 1.0)})


def submit(*, family: str, strategy: str, days: str | list[str], space: dict[str, Any] | None = None,
           search: dict[str, Any] | None = None, start: str | None = None, end: str | None = None,
           costs: Costs | dict[str, Any] | None = None, risk_free_rate: float = 0.05, iv_price_mode: str = "mid",
           note: str = "", parent: str | None = None, settings_list: list[dict[str, float]] | None = None,
           batch_size: int = BATCH_SIZE, holdout: bool = False,
           priority: int = jobs.PRIORITY_EXPERIMENT, config: str | None = None,
           check: str | None = None) -> dict[str, Any]:
    """Register an experiment and its trials, and queue the trials not yet run. Refused, with nothing
    registered, when the family's budget would be passed or a day is in a holdout (unless `holdout`: the
    promotion report, which records its use). config: the strategy/configs file it was run from, kept in its spec;
    check: the robustness check it is (analysis.robustness), kept there too, by which the promotion gates find it.
    Returns, with the counts, the trials' ids in the order of the settings."""
    if not family:
        raise ValueError("an experiment needs a family: the hypothesis its trials are variants of")
    if holdout and not access.trusted():
        raise ValueError("the holdout is the system's: holdout_report runs a version on it, once")
    if family == CONTROL_FAMILY and strategy != CONTROL[0]:
        raise ValueError(f"the family {CONTROL_FAMILY} is the control's ({CONTROL[0]}): name the hypothesis instead")
    day_files = resolve_days(days, date.fromisoformat(start) if start else None,
                             date.fromisoformat(end) if end else None, holdout=holdout)
    c = Costs.of(costs)
    settings_ = settings_list if settings_list is not None else expand(strategy, space or {}, search or {})
    if not settings_:
        raise ValueError("the experiment tries no setting")
    parameters = strategy_classes().get(strategy)
    settings_ = [resolved(strategy, s, parameters) for s in settings_]
    code = code_of(strategy)
    dh = days_hash(day_files)
    daily = sorted(runner.daily_dir().glob("*.csv")) if runner.daily_dir().is_dir() else []
    if daily:  # what the strategy may read beside the days: a change is another trial
        dh = digest([dh, days_hash(daily)])
    pricing = {"risk_free_rate": risk_free_rate, "iv_price_mode": iv_price_mode}
    trials = []
    for s in settings_:
        identity = {"strategy": strategy, "setting": s, "costs": c.model_dump(), "pricing": pricing,
                    "days": dh, "code": code}
        trials.append((digest(identity), s))
    spec = {"strategy": strategy, "space": space, "search": search, "start": start, "end": end,
            "costs": c.model_dump(), "pricing": pricing, "settings": settings_list}
    if config:
        spec["config"] = config
    if check:
        spec["check"] = check
    experiment_id = digest({"spec": spec, "family": family, "days": dh, "code": code, "at": time.time_ns()}, 12)
    with _lock:
        con = db()
        con.execute("BEGIN IMMEDIATE")
        try:
            con.execute("INSERT OR IGNORE INTO families VALUES (?, ?)", (family, DEFAULT_BUDGET))
            budget = con.execute("SELECT budget FROM families WHERE name = ?", (family,)).fetchone()[0]
            ids = [t for t, _ in trials]
            known = {r[0] for r in con.execute(
                f"SELECT id FROM trials WHERE id IN ({','.join('?' * len(ids))})", ids)}
            used = con.execute("SELECT COUNT(*) FROM trials WHERE family = ?", (family,)).fetchone()[0]
            new = [t for t in ids if t not in known]
            if used + len(new) > budget:
                raise ValueError(f"family {family} has tried {used} of its {budget} trials; this experiment "
                                 f"adds {len(new)}. Send what it has to review, give it up, or raise its "
                                 f"budget (set_family_budget)")
            con.execute("INSERT INTO experiments VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        (experiment_id, now(), family, strategy, canonical(spec),
                         json.dumps([str(f.resolve().relative_to(files.data_root().resolve())) for f in day_files]),
                         dh, canonical(code), int(holdout), parent, note))
            for position, (trial_id, s) in enumerate(trials):
                if trial_id not in known:
                    con.execute("INSERT INTO trials (id, family, strategy, setting, costs, code, status, created_at) "
                                "VALUES (?, ?, ?, ?, ?, ?, 'queued', ?)",
                                (trial_id, family, strategy, canonical(s), canonical(c.model_dump()), canonical(code),
                                 now()))
                con.execute("INSERT OR IGNORE INTO experiment_trials VALUES (?, ?, ?)",
                            (experiment_id, trial_id, position))
            # Trials run again only when they never finished: queued (in no live job) or failed.
            to_run = [t for t in ids if t not in known] + [
                r[0] for r in con.execute(
                    f"SELECT id FROM trials WHERE id IN ({','.join('?' * len(ids))}) "
                    "AND status IN ('failed', 'cancelled')", ids)]
            for t in to_run:
                con.execute("UPDATE trials SET status = 'queued', error = NULL WHERE id = ?", (t,))
            key = digest({"days": dh, "pricing": pricing})  # what a job's batch is but its trials
            for i in range(0, len(to_run), max(1, batch_size)):
                jobs.add(con, "trials", {"trial_ids": to_run[i:i + batch_size]}, priority, ref=experiment_id,
                         batch_key=key)
            con.execute("COMMIT")
        except BaseException:
            con.execute("ROLLBACK")
            raise
    return {"experiment_id": experiment_id, "trial_ids": [t for t, _ in trials], "trials": len(trials),
            "queued": len(to_run),
            "reused": len(trials) - len(to_run), "days": len(day_files), "family": family,
            "family_used": used + len(new), "family_budget": budget}


def code_of(strategy: str) -> dict[str, str]:
    """The code a strategy's trials come from, as the build says (code_identity): all a backtest is built from (core), the
    trading core within it (engine) and the strategy's own files. Refused (ValueError) when the strategy's library
    is not of its files as they are (code_identity.check_build)."""
    built = built_strategies()
    listing = {c["name"]: c for c in built["strategies"]}
    if strategy not in listing:
        raise unknown_strategy(strategy, built)
    code_identity.check_build(listing[strategy])
    library = Path(listing[strategy]["library"])
    code = {"core": code_identity.core_hash(), "engine": code_identity.engine_hash(), "strategy": code_identity.strategy_hash(library)}
    with _lock:
        db().execute("INSERT OR IGNORE INTO strategy_sources VALUES (?, ?)",
                     (code["strategy"], canonical(code_identity.strategy_sources(library))))
    return code


def strategy_source(strategy_hash: str) -> dict[str, str]:
    """The strategy's files, by name, as they were when results were credited to them (code_of keeps them)."""
    row = db().execute("SELECT files FROM strategy_sources WHERE hash = ?", (strategy_hash,)).fetchone()
    if row is None:
        raise KeyError(strategy_hash)
    return json.loads(row["files"])


def set_budget(family: str, budget: int) -> dict[str, Any]:
    if budget < 0:
        raise ValueError("a budget is not negative")
    with _lock:
        db().execute("INSERT INTO families VALUES (?, ?) ON CONFLICT(name) DO UPDATE SET budget = excluded.budget",
                     (family, budget))
    return families()[family]


def families() -> dict[str, dict[str, Any]]:
    rows = db().execute("SELECT f.name, f.budget, COUNT(t.id) AS used, "
                        "SUM(t.status = 'done') AS done FROM families f LEFT JOIN trials t ON t.family = f.name "
                        "GROUP BY f.name ORDER BY f.name").fetchall()
    return {r["name"]: {"budget": r["budget"], "used": r["used"], "done": r["done"] or 0} for r in rows}


def tries_by(experiment_id: str) -> int:
    """The trials the ledger had registered when the experiment was, in every family but the control's: what its best
    is the best of."""
    con = db()
    at = con.execute("SELECT created_at FROM experiments WHERE id = ?", (experiment_id,)).fetchone()
    if at is None:
        raise KeyError(experiment_id)
    return con.execute("SELECT COUNT(*) FROM trials WHERE created_at <= ? AND NOT (family = ? AND strategy = ?)",
                       (at[0], CONTROL_FAMILY, CONTROL[0])).fetchone()[0]


def cancel(experiment_id: str) -> dict[str, Any]:
    """Its jobs not finished are cancelled, a running one stopped; their trials are marked cancelled (they still
    count)."""
    return {"experiment_id": experiment_id, "cancelled_jobs": jobs.cancel(ref=experiment_id)}


def _code_changed(con: sqlite3.Connection, experiment_id: str) -> str | None:
    """Why the experiment's jobs cannot run now (the engine was rebuilt from other sources, or not rebuilt since its
    sources changed), or None."""
    exp = con.execute("SELECT strategy, code FROM experiments WHERE id = ?", (experiment_id,)).fetchone()
    try:
        if code_of(exp["strategy"]) != json.loads(exp["code"]):
            return "the engine or the strategy changed since the experiment was submitted: submit it again"
    except ValueError as e:
        return str(e)
    return None


# ---------------------------------------------------------------- an experiment's jobs (the kind "trials")

def _trial_ids(job: sqlite3.Row) -> list[str]:
    return json.loads(job["payload"])["trial_ids"]


def _set_trials(con: sqlite3.Connection, job: sqlite3.Row, status: str, error: str | None = None) -> None:
    ids = _trial_ids(job)
    con.execute(f"UPDATE trials SET status = ?, error = ?, finished_at = ? WHERE id IN ({','.join('?' * len(ids))}) "
                "AND status IN ('queued', 'running')",
                (status, error, now() if status in ("failed", "cancelled") else None, *ids))


def _leased(con: sqlite3.Connection, job: sqlite3.Row) -> str | None:
    # The code its trials are credited to is gone: running it would credit them with another's results.
    why = _code_changed(con, job["ref"])
    if why is None:
        _set_trials(con, job, "running")
    return why


def _batch(con: sqlite3.Connection, job: sqlite3.Row) -> dict[str, Any]:
    ids = _trial_ids(job)
    spec = json.loads(con.execute("SELECT spec FROM experiments WHERE id = ?", (job["ref"],)).fetchone()["spec"])
    trials = con.execute(f"SELECT id, strategy, setting, costs FROM trials WHERE id IN ({','.join('?' * len(ids))})",
                         ids).fetchall()
    return {"files": [str(f) for f in experiment_days(job["ref"])],
            "risk_free_rate": spec["pricing"]["risk_free_rate"], "iv_price_mode": spec["pricing"]["iv_price_mode"],
            "trials": [{"id": t["id"], "strategy": t["strategy"], "setting": json.loads(t["setting"]),
                        "costs": Costs.of(json.loads(t["costs"])).model_dump()} for t in trials]}


def _prepare(out: bytes) -> dict[str, Any]:
    """An experiment's batch, as the engine wrote it (runner.read), made ready to write before the queue's lock is
    taken: each trial's summary, errors, days and trades (trial_results), and the days' underlying."""
    days, trials = runner.read(out)
    ready, root = {}, ""
    for t in trials:
        trades, errors, notes = batch.trades_of(t)
        dated = _dated(t["days"])
        summary = summary_of(dated, trades)
        if notes:
            summary["notes"] = notes
        run_errors = list(dict.fromkeys([*(t.get("errors") or []), *errors]))
        ready[t["id"]] = (canonical(summary), "; ".join(run_errors)[:2000] or None, orjson.dumps(dated).decode(),
                          orjson.dumps(batch.trade_columns(trades)).decode())
        root = root or t["result"].get("portfolio_name") or ""
    underlying = [(iso_day(on), root, o, c) for on, o, c in
                  zip(days["date"], days["underlying_open"], days["underlying_close"]) if on and o and c]
    return {"trials": ready, "underlying": underlying}


def _dated(days: dict[str, Any]) -> dict[str, Any]:
    """A trial's days, those of files named by no date left out (the engine's date 0)."""
    keep = [i for i, on in enumerate(days["date"]) if on]
    if len(keep) == len(days["date"]):
        return days

    def take(columns: dict[str, Any]) -> dict[str, Any]:
        return {k: take(v) if isinstance(v, dict) else [v[i] for i in keep] for k, v in columns.items()}
    return take(days)


def _finished(con: sqlite3.Connection, job: sqlite3.Row, prepared: dict[str, Any]) -> None:
    for trial_id in _trial_ids(job):
        if (row := prepared["trials"].get(trial_id)) is None:
            con.execute("UPDATE trials SET status = 'failed', finished_at = ?, error = ? WHERE id = ?",
                        (now(), "not in its batch's result", trial_id))
            continue
        summary, error, days, trades = row
        con.execute("INSERT OR REPLACE INTO trial_results VALUES (?, ?, ?)", (trial_id, days, trades))
        con.execute("UPDATE trials SET status = 'done', finished_at = ?, summary = ?, error = ? WHERE id = ?",
                    (now(), summary, error, trial_id))
    # The same days for every job of the batch: written with the first.
    con.executemany("INSERT OR REPLACE INTO day_underlying VALUES (?, ?, ?, ?)", prepared["underlying"])
    prepared["underlying"] = []


def _ended(con: sqlite3.Connection, job: sqlite3.Row, status: str, error: str | None) -> None:
    if status != "done":  # done: each trial was stored as it finished
        _set_trials(con, job, status, error)


jobs.KINDS["trials"] = jobs.Kind(batch=_batch, leased=_leased, prepare=_prepare, finished=_finished, ended=_ended,
                                 requeued=lambda con, job: _set_trials(con, job, "queued"))


def summary_of(days: dict[str, Any], trades: list[batch.Trade]) -> dict[str, Any]:
    """A trial's headline numbers from its days (a column a field, as the engine gives them) and trades: net, gross,
    fees and orders, days, daily Sharpe (annualised), drawdown (of the net, day by day), its worst day and the mean
    of its worst 5% of days (ES95), the most margin and |delta|, |gamma|, |theta| of a day, trades, mean R and t. The
    one place a run's days are summed: the ledger and the page's backtest show the same."""
    net = days["net_pnl"]
    n = len(net)
    mean = sum(net) / n if n else 0.0
    sd = math.sqrt(sum((x - mean) ** 2 for x in net) / (n - 1)) if n > 1 else 0.0
    equity = peak = drawdown = 0.0
    for x in net:
        equity += x
        peak = max(peak, equity)
        drawdown = min(drawdown, equity - peak)
    rs = [t.r for t in trades]
    mean_r = sum(rs) / len(rs) if rs else None
    sd_r = math.sqrt(sum((r - mean_r) ** 2 for r in rs) / (len(rs) - 1)) if len(rs) > 1 else 0.0
    by_day = batch.day_measures([(iso_day(on), x) for on, x in zip(days["date"], net)])
    most = {k: max(days.get(k) or [0.0]) for k in ("max_margin", "max_delta", "max_gamma", "max_theta")}
    return {"net": round(sum(net), 2), "gross": round(sum(days["pnl"]), 2), "fees": round(sum(days["fees"]), 2),
            "orders": sum(days["orders"]), "days": n,
            "sharpe": round(mean / sd * math.sqrt(252), 4) if sd > 0 else 0.0,
            "drawdown": round(drawdown, 2), "worst_day": by_day.get("worst_day"), "es95_day": by_day.get("es95_day"),
            "max_margin": round(most["max_margin"], 2), "max_delta": round(most["max_delta"], 4),
            "max_gamma": round(most["max_gamma"], 4), "max_theta": round(most["max_theta"], 4),
            "trades": len(trades),
            "expectancy_r": round(mean_r, 4) if mean_r is not None else None,
            "t": round(mean_r / sd_r * math.sqrt(len(rs)), 3) if sd_r > 0 else None}


def iso_day(on: int) -> str:
    """YYYYMMDD as YYYY-MM-DD."""
    return f"{on // 10000:04d}-{on // 100 % 100:02d}-{on % 100:02d}"


def experiments(limit: int = 50) -> list[dict[str, Any]]:
    """The newest experiments, each with its trials counted by status: only those `limit` are aggregated."""
    rows = db().execute("""
        SELECT e.id, e.created_at, e.family, e.strategy, e.note, e.parent,
               COUNT(t.id) AS trials, SUM(t.status = 'done') AS done, SUM(t.status = 'failed') AS failed,
               SUM(t.status IN ('queued', 'running')) AS pending, json_array_length(e.days) AS days
        FROM (SELECT rowid AS r, * FROM experiments ORDER BY created_at DESC, rowid DESC LIMIT ?) e
        JOIN experiment_trials et ON et.experiment_id = e.id JOIN trials t ON t.id = et.trial_id
        GROUP BY e.id ORDER BY e.created_at DESC, e.r DESC""", (limit,)).fetchall()
    return [dict(r) | {"done": r["done"] or 0, "failed": r["failed"] or 0, "pending": r["pending"] or 0}
            for r in rows]


def experiments_of_config(name: str) -> list[dict[str, Any]]:
    """The experiments run from a config (strategy/configs/<name>.json), in the order they were made (created_at is to
    the second), each with its trials' count."""
    rows = db().execute("""
        SELECT e.id, e.created_at, COUNT(et.trial_id) AS trials FROM experiments e
        LEFT JOIN experiment_trials et ON et.experiment_id = e.id
        WHERE json_extract(e.spec, '$.config') = ? GROUP BY e.id ORDER BY e.rowid""", (name,)).fetchall()
    return [{"id": r["id"], "created_at": r["created_at"], "trials": r["trials"]} for r in rows]


def experiment(experiment_id: str) -> dict[str, Any]:
    con = db()
    exp = con.execute("SELECT * FROM experiments WHERE id = ?", (experiment_id,)).fetchone()
    if exp is None:
        raise KeyError(experiment_id)
    trials = con.execute("""
        SELECT t.id, t.setting, t.status, t.summary, t.error FROM experiment_trials et JOIN trials t
        ON t.id = et.trial_id WHERE et.experiment_id = ? ORDER BY et.position""", (experiment_id,)).fetchall()
    days = json.loads(exp["days"])
    return {
        "id": exp["id"], "created_at": exp["created_at"], "family": exp["family"], "strategy": exp["strategy"],
        "spec": json.loads(exp["spec"]), "days": len(days), "first_day": days[0], "last_day": days[-1],
        "code": json.loads(exp["code"]), "holdout": bool(exp["holdout"]), "days_hash": exp["days_hash"],
        "parent": exp["parent"], "note": exp["note"],
        "family_counts": families().get(exp["family"]),
        "jobs": jobs.counts(experiment_id),
        "trials": [{"id": t["id"], "setting": json.loads(t["setting"]), "status": t["status"],
                    "summary": json.loads(t["summary"]) if t["summary"] else None, "error": t["error"]}
                   for t in trials],
    }


def experiments_of(trial_id: str) -> list[str]:
    """The experiments that hold the trial, the first first."""
    rows = db().execute("SELECT e.id FROM experiment_trials et JOIN experiments e ON e.id = et.experiment_id "
                        "WHERE et.trial_id = ? ORDER BY e.created_at, e.rowid", (trial_id,)).fetchall()
    if not rows:
        raise KeyError(trial_id)
    return [r[0] for r in rows]


def experiment_days(experiment_id: str) -> list[Path]:
    """The experiment's day files (the ledger keeps them relative to data/)."""
    row = db().execute("SELECT days FROM experiments WHERE id = ?", (experiment_id,)).fetchone()
    if row is None:
        raise KeyError(experiment_id)
    root = files.data_root().resolve()
    return [root / f for f in json.loads(row[0])]


def trial_ids(experiment_id: str, done_only: bool = True) -> list[str]:
    q = ("SELECT t.id FROM experiment_trials et JOIN trials t ON t.id = et.trial_id WHERE et.experiment_id = ?"
         + (" AND t.status = 'done'" if done_only else "") + " ORDER BY et.position")
    return [r[0] for r in db().execute(q, (experiment_id,))]


def _results(ids: list[str], column: str) -> dict[str, dict[str, Any]]:
    """Of each finished trial of these, its days or its trades as stored: a column a field."""
    out = {}
    for chunk in (ids[i:i + 500] for i in range(0, len(ids), 500)):
        for r in db().execute(f"SELECT trial_id, {column} FROM trial_results WHERE trial_id IN "
                              f"({','.join('?' * len(chunk))})", chunk):
            out[r[0]] = orjson.loads(r[1])
    return out


def _rows(trial_id: str, columns: dict[str, Any]) -> list[dict[str, Any]]:
    return [{"trial_id": trial_id, **row} for row in runner.rows(columns)]


def trial_days(ids: Iterable[str]) -> dict[str, list[dict[str, Any]]]:
    """Each trial's days in order: {"trial_id", "day" (YYYY-MM-DD), "pnl", "net_pnl", "fees", "orders", "flat", the
    P&L by source (batch.ATTRIBUTION)}."""
    ids = list(ids)
    out: dict[str, list[dict[str, Any]]] = {i: [] for i in ids}
    for trial_id, days in _results(ids, "days").items():
        rows = _rows(trial_id, days)
        for row in rows:
            row["day"] = iso_day(row.pop("date"))
        out[trial_id] = rows
    return out


def trial(trial_id: str) -> dict[str, Any]:
    """A trial: its strategy, setting, costs, code, status, summary."""
    row = db().execute("SELECT * FROM trials WHERE id = ?", (trial_id,)).fetchone()
    if row is None:
        raise KeyError(trial_id)
    return {**dict(row), **{k: json.loads(row[k]) for k in ("setting", "costs", "code")},
            "summary": json.loads(row["summary"]) if row["summary"] else None}


def trial_errors(ids: Iterable[str]) -> dict[str, str]:
    """What went wrong in each of these trials, for those where something did: a run that failed, or stopped before
    its last day, or whose strategy had an error (trades only left out are a trial's notes, its summary's)."""
    ids = list(ids)
    out = {}
    for chunk in (ids[i:i + 500] for i in range(0, len(ids), 500)):
        for r in db().execute(f"SELECT id, error FROM trials WHERE id IN ({','.join('?' * len(chunk))}) "
                              "AND error IS NOT NULL", chunk):
            out[r["id"]] = r["error"]
    return out


def trial_trades(ids: Iterable[str]) -> dict[str, list[dict[str, Any]]]:
    """Each trial's trades by when they opened: {"trial_id", "opened" (YYYY-MM-DD), "days", "net", "risk", "r",
    "hedge_fills", the P&L by source, "fees", "entry_cost", "exit_cost", "settled"} (batch.trade_columns)."""
    ids = list(ids)
    out: dict[str, list[dict[str, Any]]] = {i: [] for i in ids}
    for trial_id, trades in _results(ids, "trades").items():
        out[trial_id] = _rows(trial_id, trades)
    return out


def matrix(experiment_id: str) -> tuple[list[str], list[str], np.ndarray]:
    """The experiment's finished trials by its days: (trial ids, days, net P&L [trial, day] as float64); a day a trial
    did not run is NaN."""
    ids = trial_ids(experiment_id)
    stored = _results(ids, "days")
    dates = sorted({on for days in stored.values() for on in days["date"]})
    index = {on: i for i, on in enumerate(dates)}
    values = np.full((len(ids), len(dates)), np.nan)
    for row, t in enumerate(ids):
        if (days := stored.get(t)) is not None:
            values[row, [index[on] for on in days["date"]]] = days["net_pnl"]
    return ids, [iso_day(on) for on in dates], values


def day_moves(days: list[str]) -> dict[str, float]:
    """Each day's move of the underlying, open to close, as the engine reported it."""
    out = {}
    for chunk in (days[i:i + 500] for i in range(0, len(days), 500)):
        for r in db().execute(f"SELECT day, open, close FROM day_underlying WHERE day IN "
                              f"({','.join('?' * len(chunk))})", chunk):
            if r["open"] > 0:
                out[r["day"]] = r["close"] / r["open"] - 1
    return out


def check_trials(experiment_id: str, check: str) -> list[dict[str, Any]]:
    """The trials of the robustness checks of an experiment registered as `check` (analysis.robustness), finished or
    not: each with its setting, status and summary (None until it is done)."""
    rows = db().execute("""
        SELECT t.id, t.setting, t.status, t.summary FROM experiments e
        JOIN experiment_trials et ON et.experiment_id = e.id JOIN trials t ON t.id = et.trial_id
        WHERE e.parent = ? AND json_extract(e.spec, '$.check') = ?
        ORDER BY e.rowid, et.position""", (experiment_id, check)).fetchall()
    return [{"id": r["id"], "setting": json.loads(r["setting"]), "status": r["status"],
             "summary": json.loads(r["summary"]) if r["summary"] else None} for r in rows]
