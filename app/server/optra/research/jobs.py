"""The queue of heavy runs. Every backtest the server needs (an experiment's trials, the page's backtest, the promotion
report, a hedge study, the daily report's replay) is a job here, which a worker (research.worker) runs in the research
slice: the server's own process never starts one.

A job has a kind, a priority and a payload. A worker leases the most urgent job it takes (a worker may take only jobs
from a priority up: the lane kept for someone waiting), renews the lease every RENEW_SECONDS while it runs, and reports
the result. Jobs queued with the same batch key (the same days, priced the same) and priority run as one batch: the
worker leases them together under the first (their lead), up to BATCH_TRIALS trials, each day read once for all; the
lease, the result and handing back are the group's, the result split back by trial. A job cancelled while it runs is told to its worker at the next renewal, which stops the run. A job whose
worker vanished (the lease ran out: the worker was killed, most often for memory) is offered again until MAX_ATTEMPTS,
then fails saying so; a run that failed is not tried again.

The kinds: "batch", a run whose trials' results go back to whoever asked (run()), and "trials", an experiment's
(the ledger registers the kind: it stores the results, see ledger.py).

Outside the server (the tests, a script) there are no workers: run() and wait() run the jobs in the calling
process (here mode). The server switches to the workers when it starts (use_workers)."""

from __future__ import annotations

import sqlite3
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable

import orjson

from optra import settings
from optra.research import runner, store
from optra.research.runner import Cancelled

PRIORITY_EXPERIMENT = 0
PRIORITY_REPORT = 10  # the promotion report, a hedge study, the daily report: a tool call waits for it
PRIORITY_PAGE = 20    # the page's backtest: someone is watching
MAX_ATTEMPTS = 2
LEASE_SECONDS = 120
RENEW_SECONDS = 5
WORKER_GONE_SECONDS = 60   # a worker not heard from for this long is not counted on
NO_WORKER_SECONDS = 30     # a job no live worker takes fails after waiting this long
BATCH_TRIALS = 512         # config.json "batch_trials": the most trials jobs leased together run in one batch


@dataclass
class Kind:
    """What a kind of job does at each step, inside the queue's transaction; but prepare, which makes the result
    ready for finished before the queue's lock is taken."""
    batch: Callable[[sqlite3.Connection, sqlite3.Row], dict[str, Any]]  # what the worker runs
    leased: Callable[[sqlite3.Connection, sqlite3.Row], str | None] = lambda con, job: None  # why not now, or None
    prepare: Callable[[bytes], Any] = lambda out: out  # the engine's output (runner.read) for finished
    finished: Callable[[sqlite3.Connection, sqlite3.Row, Any], None] = lambda con, job, prepared: None
    ended: Callable[[sqlite3.Connection, sqlite3.Row, str, str | None], None] = lambda con, job, st, err: None
    requeued: Callable[[sqlite3.Connection, sqlite3.Row], None] = lambda con, job: None


def _store_result(con: sqlite3.Connection, job: sqlite3.Row, out: bytes) -> None:
    con.execute("UPDATE jobs SET result = ? WHERE id = ?", (out, job["id"]))  # as the engine wrote it


KINDS: dict[str, Kind] = {"batch": Kind(batch=lambda con, job: orjson.loads(job["payload"]), finished=_store_result)}

_here = True
_workers: dict[str, tuple[float, int]] = {}  # a worker's name -> (when last heard from, the lowest priority it takes)
_workers_lock = threading.Lock()


def is_here() -> bool:
    """Jobs run in this process (no workers: the tests, a script)."""
    return _here


def use_workers(on: bool = True) -> None:
    """Jobs wait for the workers (the server, while it runs), or run in this process again."""
    global _here
    _here = not on


# ---------------------------------------------------------------- the queue

def add(con: sqlite3.Connection, kind: str, payload: dict[str, Any], priority: int, ref: str | None = None,
        name: str | None = None, batch_key: str | None = None) -> int:
    """Queues a job inside the caller's transaction; its id. Jobs of a kind with the same batch_key can run as one
    batch (its days and how they are priced: the kind's batch of each differs only in its trials)."""
    return con.execute("INSERT INTO jobs (kind, ref, name, priority, payload, status, created_at, batch_key) "
                       "VALUES (?, ?, ?, ?, ?, 'queued', ?, ?)",
                       (kind, ref, name, priority, orjson.dumps(payload).decode(), store.now(), batch_key)).lastrowid


def _end(con: sqlite3.Connection, job: sqlite3.Row, status: str, error: str | None) -> None:
    con.execute("UPDATE jobs SET status = ?, finished_at = ?, error = ?, lease_until = NULL WHERE id = ?",
                (status, store.now(), error, job["id"]))
    KINDS[job["kind"]].ended(con, job, status, error)


def _expire(con: sqlite3.Connection) -> None:
    """Leases run out: their jobs offered again, or failed once they have had their attempts."""
    for job in con.execute("SELECT * FROM jobs WHERE status = 'leased' AND lease_until < ?", (time.time(),)).fetchall():
        if job["attempts"] >= MAX_ATTEMPTS:
            _end(con, job, "failed", f"its worker stopped before it finished, {job['attempts']} times (killed: most "
                                     "often out of memory, the batch too large for the research slice)")
        else:
            con.execute("UPDATE jobs SET status = 'queued', worker = NULL, lease_until = NULL, lead = NULL "
                        "WHERE id = ?", (job["id"],))
            KINDS[job["kind"]].requeued(con, job)


def _heard(worker: str, min_priority: int | None = None) -> None:
    with _workers_lock:
        known = _workers.get(worker)
        _workers[worker] = (time.monotonic(), min_priority if min_priority is not None else
                            (known[1] if known else 0))


def workers() -> dict[str, dict[str, Any]]:
    """The workers heard from lately: when, and the lowest priority each takes."""
    t = time.monotonic()
    with _workers_lock:
        return {w: {"seconds_ago": round(t - at, 1), "min_priority": p} for w, (at, p) in _workers.items()
                if t - at < WORKER_GONE_SECONDS}


def lease(worker: str, min_priority: int = 0, ref: str | None = None) -> dict[str, Any] | None:
    """The most urgent job queued that this worker takes (from min_priority up; of one experiment with ref), leased
    to it: {"job_id", "kind", "lease_seconds", "renew_seconds", "batch"}; None when there is none."""
    _heard(worker, min_priority)
    with store.lock:
        con = store.db()
        while True:
            con.execute("BEGIN IMMEDIATE")
            try:
                _expire(con)
                job = con.execute("SELECT * FROM jobs WHERE status = 'queued' AND priority >= ? "
                                  "AND (? IS NULL OR ref = ?) ORDER BY priority DESC, id LIMIT 1",
                                  (min_priority, ref, ref)).fetchone()
                if job is None:
                    con.execute("COMMIT")
                    return None
                kind = KINDS[job["kind"]]
                why = kind.leased(con, job)
                if why is not None:
                    _end(con, job, "failed", why)
                    con.execute("COMMIT")
                    continue
                batch = kind.batch(con, job)
                group = [job, *_joining(con, job, kind, batch)]
                for member in group:
                    con.execute("UPDATE jobs SET status = 'leased', worker = ?, lease_until = ?, "
                                "attempts = attempts + 1, lead = ? WHERE id = ?",
                                (worker, time.time() + LEASE_SECONDS, job["id"], member["id"]))
                con.execute("COMMIT")
            except BaseException:
                con.execute("ROLLBACK")
                raise
            return {"job_id": job["id"], "kind": job["kind"], "lease_seconds": LEASE_SECONDS,
                    "renew_seconds": RENEW_SECONDS, "batch": batch}


def _joining(con: sqlite3.Connection, lead: sqlite3.Row, kind: Kind, batch: dict[str, Any]) -> list[sqlite3.Row]:
    """The jobs queued that run in the lead's batch (the same kind, priority and batch key), their trials added to
    it, up to BATCH_TRIALS in all; one that cannot run now ends failed."""
    if lead["batch_key"] is None:
        return []
    most = int(settings.config().get("batch_trials", BATCH_TRIALS))
    joining = []
    for job in con.execute("SELECT * FROM jobs WHERE status = 'queued' AND kind = ? AND priority = ? "
                           "AND batch_key = ? AND id != ? ORDER BY id",
                           (lead["kind"], lead["priority"], lead["batch_key"], lead["id"])).fetchall():
        trials = kind.batch(con, job)["trials"]
        if len(batch["trials"]) + len(trials) > most:
            break
        if (why := kind.leased(con, job)) is not None:
            _end(con, job, "failed", why)
            continue
        batch["trials"] += trials
        joining.append(job)
    return joining


def _group(con: sqlite3.Connection, job_id: int, worker: str) -> list[sqlite3.Row]:
    """The job and those leased with it (it their lead), as far as they are this worker's still."""
    return con.execute("SELECT * FROM jobs WHERE (id = ? OR lead = ?) AND worker = ? AND status = 'leased' "
                       "ORDER BY id", (job_id, job_id, worker)).fetchall()


def renew(job_id: int, worker: str) -> str:
    """ok (the lease runs on), cancelled (stop the run: every job of its group was cancelled) or lost (the job is no
    longer this worker's)."""
    _heard(worker)
    with store.lock:
        con = store.db()
        if _group(con, job_id, worker):
            con.execute("UPDATE jobs SET lease_until = ? WHERE (id = ? OR lead = ?) AND worker = ? "
                        "AND status = 'leased'", (time.time() + LEASE_SECONDS, job_id, job_id, worker))
            return "ok"
        cancelled = con.execute("SELECT 1 FROM jobs WHERE (id = ? OR lead = ?) AND worker = ? "
                                "AND status = 'cancelled'", (job_id, job_id, worker)).fetchone()
    return "cancelled" if cancelled else "lost"


def release(job_id: int, worker: str) -> bool:
    """A worker stopping hands its job back, and those leased with it: queued again at once, the attempt not counted
    (it was not the jobs' fault), rather than waiting for the lease to lapse. False when none is leased to it."""
    _heard(worker)
    with store.lock:
        con = store.db()
        con.execute("BEGIN IMMEDIATE")
        try:
            group = _group(con, job_id, worker)
            for job in group:
                con.execute("UPDATE jobs SET status = 'queued', worker = NULL, lease_until = NULL, lead = NULL, "
                            "attempts = attempts - 1 WHERE id = ?", (job["id"],))
                KINDS[job["kind"]].requeued(con, job)
            con.execute("COMMIT")
            if not group:
                return False
        except BaseException:
            con.execute("ROLLBACK")
            raise
    return True


def complete(job_id: int, worker: str, out: bytes) -> dict[str, Any]:
    """A worker's result for its job and those leased with it: the engine's output (runner.read), or {"status":
    "error", "error"}. Taken apart and made ready (Kind.prepare) before the queue's lock is taken; inside it, only
    written."""
    _heard(worker)
    with store.lock:
        job = store.db().execute("SELECT kind FROM jobs WHERE id = ?", (job_id,)).fetchone()
    error, prepared = None, None
    try:
        runner.read(out)  # its last line: whether the run went through
        if job is not None:
            prepared = KINDS[job["kind"]].prepare(out)
    except ValueError as e:
        error = str(e)[:2000] or "the batch failed"
    with store.lock:
        con = store.db()
        con.execute("BEGIN IMMEDIATE")
        try:
            group = _group(con, job_id, worker)
            if not group:
                con.execute("COMMIT")
                return {"accepted": False, "why": "the job is not leased to this worker (cancelled, or offered again)"}
            for member in group:
                if error is None:
                    KINDS[member["kind"]].finished(con, member, prepared)
                    _end(con, member, "done", None)
                else:
                    _end(con, member, "failed", error)
            con.execute("COMMIT")
        except BaseException:
            con.execute("ROLLBACK")
            raise
    return {"accepted": True}


def cancel(*, job_id: int | None = None, ref: str | None = None, name: str | None = None) -> int:
    """Cancels the jobs not finished of that id, experiment or name: a queued one does not run, a leased one stops at
    its worker's next renewal. How many."""
    with store.lock:
        con = store.db()
        con.execute("BEGIN IMMEDIATE")
        try:
            jobs = con.execute("SELECT * FROM jobs WHERE status IN ('queued', 'leased') AND (id = ? OR ref = ? OR "
                               "name = ?)", (job_id, ref, name)).fetchall()
            for job in jobs:
                _end(con, job, "cancelled", "cancelled")
            con.execute("COMMIT")
        except BaseException:
            con.execute("ROLLBACK")
            raise
    if name is not None:
        runner.cancel(name)  # here mode: the run in this process
    return len(jobs)


def counts(ref: str) -> dict[str, int]:
    """An experiment's jobs by status."""
    with store.lock:
        rows = store.db().execute("SELECT status, COUNT(*) AS n FROM jobs WHERE ref = ? GROUP BY status", (ref,))
        return {r["status"]: r["n"] for r in rows}


# ---------------------------------------------------------------- waiting for jobs

def _run_here(job: dict[str, Any], name: str | None = None) -> bytes:
    b = job["batch"]
    try:
        return runner.run_trials(b["files"], b["trials"], b["risk_free_rate"], b["iv_price_mode"], name=name)
    except ValueError as e:
        return orjson.dumps({"status": "error", "error": str(e)})


def _taken(priority: int) -> bool:
    return any(w["min_priority"] <= priority for w in workers().values())


def wait(*, ref: str | None = None, job_ids: list[int] | None = None, timeout: float = 6 * 3600) -> None:
    """Until the jobs of an experiment (ref) or these jobs have ended. Here mode: they run now, in this process."""
    if _here:
        worker = f"here:{threading.get_ident()}"
        while (job := lease(worker, ref=ref)) is not None:
            complete(job["job_id"], worker, _run_here(job))
        return
    where, args = ("ref = ?", [ref]) if ref is not None else (
        f"id IN ({','.join('?' * len(job_ids or []))})", list(job_ids or []))
    started = time.monotonic()
    while True:
        with store.lock:
            con = store.db()
            con.execute("BEGIN IMMEDIATE")
            _expire(con)  # a worker that died with one of them: no lease of another may come to notice it
            con.execute("COMMIT")
            open_ = con.execute(f"SELECT id, priority, status FROM jobs WHERE {where} "
                                "AND status IN ('queued', 'leased')", args).fetchall()
        if not open_:
            return
        waited = time.monotonic() - started
        if waited > timeout:
            raise ValueError(f"the jobs did not finish in {timeout:.0f} s")
        queued = [j for j in open_ if j["status"] == "queued"]
        if queued and waited > NO_WORKER_SECONDS and not _taken(min(j["priority"] for j in queued)):
            for j in queued:
                cancel(job_id=j["id"])
            raise ValueError("no research worker takes these jobs: start them (systemctl --user start "
                             "optra-worker@interactive optra-worker@bulk)")
        time.sleep(0.25)


def submit(batch: dict[str, Any], priority: int, name: str | None = None) -> int:
    """A batch queued as a job, not waited for (run() waits); a name stops the run of that name before. Its id."""
    if name is not None:
        cancel(name=name)
    with store.lock:
        return add(store.db(), "batch", batch, priority, name=name)


def run(batch: dict[str, Any], priority: int, name: str | None = None, timeout: float = 6 * 3600
        ) -> tuple[dict[str, list[Any]], list[dict[str, Any]]]:
    """A batch ({"files", "trials", "risk_free_rate", "iv_price_mode"}) as a job, waited for: the days its trials ran
    and each trial's result, as runner.read gives them. A name stops the run of that name before (the page's
    backtest). Raises Cancelled when it was cancelled, ValueError when it failed."""
    if _here:
        days, trials = runner.read(_run_here({"batch": batch}, name))
        return days, list(trials)
    job_id = submit(batch, priority, name)
    try:
        wait(job_ids=[job_id], timeout=timeout)
    except BaseException:
        cancel(job_id=job_id)
        raise
    with store.lock:
        con = store.db()
        job = con.execute("SELECT status, error, result FROM jobs WHERE id = ?", (job_id,)).fetchone()
        con.execute("UPDATE jobs SET result = NULL WHERE id = ?", (job_id,))
    if job["status"] == "cancelled":
        raise Cancelled(name or str(job_id))
    if job["status"] != "done":
        raise ValueError(job["error"] or "the job failed")
    days, trials = runner.read(job["result"])
    return days, list(trials)
