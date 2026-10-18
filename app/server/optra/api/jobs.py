"""The workers' side of the job queue (research.worker, with the key: infra/access.py): lease a job, renew the lease while it runs (and learn there
that it was cancelled), report its result or hand it back when stopping; and what the queue holds. Everything else about experiments is a tool
(optra.tools.experiments: /api/experiments...)."""

from __future__ import annotations

import asyncio
import time
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import BaseModel

from optra.infra import access
from optra.research import jobs, store
from optra.research import ledger  # noqa: F401  (it registers the kind "trials")

router = APIRouter(prefix="/api/jobs", tags=["jobs"])
ANYONE = [Depends(access.anyone)]

LONG_POLL_SECONDS = 20.0
SYSTEM = [Depends(access.system_only)]  # a result handed in is a trial's: the workers' only


class Lease(BaseModel):
    worker: str
    min_priority: int = 0


class Worker(BaseModel):
    worker: str


@router.post("/lease", dependencies=SYSTEM)
async def lease(body: Lease) -> dict[str, Any]:
    """The most urgent job this worker takes, leased to it; {"job": null} when none came in LONG_POLL_SECONDS."""
    until = time.monotonic() + LONG_POLL_SECONDS
    while True:
        job = await asyncio.to_thread(jobs.lease, body.worker, body.min_priority)
        if job is not None or time.monotonic() > until:
            return {"job": job}
        await asyncio.sleep(0.5)


@router.post("/{job_id}/renew", dependencies=SYSTEM)
async def renew(job_id: int, body: Worker) -> dict[str, str]:
    """{"status": "ok"}, "cancelled" (stop the run) or "lost" (the job is no longer this worker's)."""
    return {"status": await asyncio.to_thread(jobs.renew, job_id, body.worker)}


@router.post("/{job_id}/release", dependencies=SYSTEM)
async def release(job_id: int, body: Worker) -> dict[str, bool]:
    """{"released": true}: queued again for another worker; false when it was not this worker's."""
    return {"released": await asyncio.to_thread(jobs.release, job_id, body.worker)}


@router.post("/{job_id}/result", dependencies=SYSTEM)
async def result(job_id: int, worker: str, request: Request) -> dict[str, Any]:
    """The job's result as the engine wrote it (runner.read), or {"status": "error", "error"}: the body read whole,
    then taken apart in a thread, not on the event loop."""
    body = await request.body()
    out = await asyncio.to_thread(jobs.complete, job_id, worker, body)
    if not out["accepted"]:
        raise HTTPException(409, out["why"])
    return out


@router.get("", dependencies=ANYONE)
async def queue() -> dict[str, Any]:
    """The workers heard from lately, and the jobs not finished by kind and status."""
    def counts() -> list[dict[str, Any]]:
        with store.lock:
            rows = store.db().execute("SELECT kind, status, priority, COUNT(*) AS n FROM jobs "
                                      "WHERE status IN ('queued', 'leased') GROUP BY kind, status, priority")
            return [dict(r) for r in rows]
    return {"workers": jobs.workers(), "open": await asyncio.to_thread(counts)}

