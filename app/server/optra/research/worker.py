"""A worker of the job queue (research.jobs): leases a job from Optra's server, runs its batch with entry_backtest
--batch (research.runner), reports the result, and leases the next. It keeps nothing of its own, so any number can
run, here or on another Linux machine with the same build, the same data/ (a job names the files by path) and the
server's key (OPTRA_HOME/access.key: what it hands back is trusted as trials' results, infra/access.py).

    uv run python -m optra.research.worker [--server http://127.0.0.1:8080] [--lane interactive|bulk]
                                           [--threads N] [--days-ahead N] [--once]

The interactive lane keeps a worker for the jobs someone waits for (the page's backtest, the reports): it takes none
below their priority; bulk (the default) takes any, the most urgent first.
While a job runs its lease is renewed every few seconds; told there that the job was cancelled (or is no longer its
own), the worker stops the run. Stopped (SIGTERM, as systemd stops it), it stops its run and hands the job back to be
offered again at once; one that dies lets the lease lapse, and the job is offered again then.
"""

from __future__ import annotations

import argparse
import os
import signal
import socket
import sys
import threading
import time
from typing import Any

import httpx
import orjson

from optra.infra import access
from optra.research import jobs, runner

LANES = {"interactive": jobs.PRIORITY_REPORT, "bulk": jobs.PRIORITY_EXPERIMENT}  # the lowest priority each takes


def run_job(job: dict[str, Any], threads: int, days_ahead: int, name: str | None = None) -> bytes:
    """The job's batch through runner.run_trials: what the server takes back, the engine's output as it wrote it, or
    {"status": "error", "error"}."""
    b = job["batch"]
    try:
        return runner.run_trials(b["files"], b["trials"], b["risk_free_rate"], b["iv_price_mode"], threads,
                                 days_ahead, name=name)
    except ValueError as e:
        return orjson.dumps({"status": "error", "error": str(e)})


def keep_lease(client: httpx.Client, job: dict[str, Any], worker: str, run_name: str, done: threading.Event) -> None:
    """Renews the job's lease until done; stops the run when the server says it is cancelled or no longer ours."""
    while not done.wait(job["renew_seconds"]):
        try:
            status = client.post(f"/api/jobs/{job['job_id']}/renew", json={"worker": worker}).json()["status"]
        except (httpx.HTTPError, ValueError, KeyError):
            continue  # the server restarting: the lease outlasts a few missed renewals
        if status != "ok":
            print(f"job {job['job_id']}: {status}, stopping it", flush=True)
            runner.cancel(run_name)
            return


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--server", default=os.environ.get("OPTRA_SERVER", "http://127.0.0.1:8080"))
    parser.add_argument("--lane", choices=LANES, default="bulk", help="interactive: only the jobs someone waits for")
    parser.add_argument("--threads", type=int, default=0, help="threads for a batch (0: one per core)")
    parser.add_argument("--days-ahead", type=int, default=3, help="days loaded ahead from the cache (one from a file)")
    parser.add_argument("--once", action="store_true", help="run the jobs queued, then stop")
    args = parser.parse_args()
    worker = f"{socket.gethostname()}:{os.getpid()}"
    client = httpx.Client(base_url=args.server, timeout=60, headers={access.HEADER: access.key()})  # the system's
    min_priority = LANES[args.lane]
    print(f"worker {worker} ({args.lane}) on {args.server}", flush=True)
    running: dict[str, str] = {}  # the run under way, by name
    stopping = threading.Event()

    def stop(signum: int, _frame: Any) -> None:
        stopping.set()
        if not running:
            raise SystemExit(0)  # waiting for a job: nothing to hand back
        # not here: the handler runs on the main thread, which may hold the runner's lock
        threading.Thread(target=runner.cancel, args=(running["name"],), daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    while not stopping.is_set():
        try:
            r = client.post("/api/jobs/lease", json={"worker": worker, "min_priority": min_priority})
            r.raise_for_status()
            job = r.json()["job"]
        except httpx.HTTPError as e:
            print(f"cannot reach the server: {e}", file=sys.stderr, flush=True)
            if args.once:
                return 1
            time.sleep(5)
            continue
        if job is None:
            if args.once:
                return 0
            continue  # the lease is a long poll: ask again at once
        job_id = job["job_id"]
        if stopping.is_set():  # leased as it was told to stop
            client.post(f"/api/jobs/{job_id}/release", json={"worker": worker})
            break
        print(f"job {job_id} ({job['kind']}): {len(job['batch']['trials'])} trials over "
              f"{len(job['batch']['files'])} days", flush=True)
        done = threading.Event()
        run_name = f"job-{job_id}"
        renewer = threading.Thread(target=keep_lease, args=(client, job, worker, run_name, done), daemon=True)
        renewer.start()
        started = time.time()
        running["name"] = run_name
        try:
            result = run_job(job, args.threads, args.days_ahead, run_name)
        except runner.Cancelled:
            print(f"job {job_id}: stopped in {time.time() - started:.0f} s", flush=True)
            if stopping.is_set():
                try:
                    client.post(f"/api/jobs/{job_id}/release", json={"worker": worker})
                    print(f"job {job_id}: handed back", flush=True)
                except httpx.HTTPError:
                    pass  # the lease lapses instead
            continue
        except Exception as e:  # noqa: BLE001 - whatever stopped the run is the job's error
            result = orjson.dumps({"status": "error", "error": f"{type(e).__name__}: {e}"})
        finally:
            done.set()
            running.clear()
        for attempt in range(5):
            try:
                r = client.post(f"/api/jobs/{job_id}/result", params={"worker": worker}, content=result,
                                headers={"content-type": "application/json"})
                if r.status_code != 409:  # 409: cancelled or offered again meanwhile, nothing to report
                    r.raise_for_status()
                break
            except httpx.HTTPError as e:
                print(f"job {job_id}: cannot report ({e}), again", file=sys.stderr, flush=True)
                time.sleep(2 ** attempt)
        print(f"job {job_id}: {len(result):,} bytes in {time.time() - started:.0f} s", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
