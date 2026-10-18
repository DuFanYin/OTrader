"""The job queue on its own: the most urgent job first and the lanes, a lease that runs out offered again and then
failed, a job handed back by a worker stopping, cancelling a running job, a failed run not tried again, waiting with and without a worker; and the runner's
process stopped from outside, by cancel or by the kernel."""

from __future__ import annotations

import os
import signal
import threading
import time

import orjson
import pytest

from optra.research import jobs, runner, store, strategies, code_identity
from optra.utils import files


@pytest.fixture(autouse=True)
def queue(tmp_path, monkeypatch):
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    monkeypatch.setattr(store, "_db", None)
    monkeypatch.setattr(jobs, "_workers", {})
    yield
    jobs._here = True


# The engine's output of a batch whose trials ran no day: its last line only.
OK = b'{"status":"ok","days":{"date":[],"file":[],"bars":[],"quotes":[],"underlying_open":[],"underlying_close":[]}}\n'


def add(priority: int, trials: list[dict] | None = None, **kw) -> int:
    with store.lock:
        return jobs.add(store.db(), "batch", {"files": [], "trials": trials or [], "risk_free_rate": 0.05,
                                              "iv_price_mode": "mid"}, priority, **kw)


def status(job_id: int) -> tuple[str, str | None]:
    row = store.db().execute("SELECT status, error FROM jobs WHERE id = ?", (job_id,)).fetchone()
    return row["status"], row["error"]


def expire(job_id: int) -> None:
    store.db().execute("UPDATE jobs SET lease_until = 0 WHERE id = ?", (job_id,))


def test_the_most_urgent_job_goes_first_and_a_lane_takes_none_below_it():
    bulk, page, report = add(jobs.PRIORITY_EXPERIMENT), add(jobs.PRIORITY_PAGE), add(jobs.PRIORITY_REPORT)
    assert jobs.lease("w1", jobs.PRIORITY_REPORT)["job_id"] == page
    assert jobs.lease("w1", jobs.PRIORITY_REPORT)["job_id"] == report
    assert jobs.lease("w1", jobs.PRIORITY_REPORT) is None  # the interactive lane leaves the experiment
    assert jobs.lease("w2")["job_id"] == bulk
    assert set(jobs.workers()) == {"w1", "w2"}


def test_a_lease_that_runs_out_is_offered_again_then_fails_saying_why():
    job = add(0)
    for attempt in range(jobs.MAX_ATTEMPTS):
        assert jobs.lease("w")["job_id"] == job
        expire(job)  # the worker was killed: it never renews
    assert jobs.lease("w") is None
    state, error = status(job)
    assert state == "failed" and "stopped before it finished, 2 times" in error


def test_a_job_handed_back_by_a_worker_stopping_is_offered_again_at_once_its_attempt_not_counted():
    job = add(0)
    jobs.lease("w")
    assert not jobs.release(job, "other")  # not its job
    assert jobs.release(job, "w")
    assert status(job)[0] == "queued" and not jobs.release(job, "w")
    for _ in range(jobs.MAX_ATTEMPTS + 1):  # handed back as often as it is stopped: never failed for it
        assert jobs.lease("w2")["job_id"] == job
        assert jobs.release(job, "w2")
    assert status(job)[0] == "queued"


def test_a_job_cancelled_while_it_runs_is_told_at_the_renewal_and_its_result_refused():
    job = add(0)
    jobs.lease("w")
    assert jobs.renew(job, "w") == "ok"
    assert jobs.renew(job, "other") == "lost"
    assert jobs.cancel(job_id=job) == 1
    assert jobs.renew(job, "w") == "cancelled"
    assert not jobs.complete(job, "w", OK)["accepted"]
    assert status(job)[0] == "cancelled"


def test_jobs_of_the_same_days_run_as_one_batch_and_end_together():
    a = add(0, [{"id": "a"}], batch_key="k")
    b = add(0, [{"id": "b"}], batch_key="k")
    other = add(0, [{"id": "c"}], batch_key="another")
    urgent = add(jobs.PRIORITY_REPORT, [{"id": "d"}], batch_key="k")
    assert [t["id"] for t in jobs.lease("w")["batch"]["trials"]] == ["d"]  # another priority: alone
    job = jobs.lease("w")
    assert job["job_id"] == a and [t["id"] for t in job["batch"]["trials"]] == ["a", "b"]
    assert status(b)[0] == "leased" and status(other)[0] == "queued"
    jobs.cancel(job_id=a)
    assert jobs.renew(a, "w") == "ok"  # b still wants the run
    assert jobs.complete(a, "w", OK)["accepted"]
    assert status(a)[0] == "cancelled" and status(b)[0] == "done"
    assert status(urgent)[0] == "leased"


def test_jobs_leased_together_are_handed_back_together():
    a = add(0, [{"id": "a"}], batch_key="k")
    b = add(0, [{"id": "b"}], batch_key="k")
    jobs.lease("w")
    assert jobs.release(a, "w")
    assert status(a)[0] == status(b)[0] == "queued"
    jobs.cancel(job_id=a)
    jobs.cancel(job_id=b)
    jobs.lease("w")  # nothing left
    assert jobs.renew(a, "w") == "lost"


def test_a_run_that_failed_is_not_tried_again():
    job = add(0)
    jobs.lease("w")
    assert jobs.complete(job, "w", orjson.dumps({"status": "error", "error": "the batch broke"}))["accepted"]
    assert status(job) == ("failed", "the batch broke")
    assert jobs.lease("w") is None


def test_a_name_stops_the_job_of_that_name_before():
    first = add(jobs.PRIORITY_PAGE, name="page")
    second = jobs.submit({"files": [], "trials": [], "risk_free_rate": 0.05, "iv_price_mode": "mid"},
                         jobs.PRIORITY_PAGE, name="page")
    assert status(first)[0] == "cancelled" and status(second)[0] == "queued"


def test_run_waits_for_a_worker_and_takes_its_result_once():
    jobs.use_workers()

    def worker() -> None:
        while (job := jobs.lease("w")) is None:
            time.sleep(0.05)
        jobs.complete(job["job_id"], "w", b'{"id":"t","net":1.5}\n' + OK)
    t = threading.Thread(target=worker)
    t.start()
    assert jobs.run({"files": [], "trials": [], "risk_free_rate": 0.05, "iv_price_mode": "mid"},
                    jobs.PRIORITY_PAGE)[1] == [{"id": "t", "net": 1.5}]
    t.join()
    assert store.db().execute("SELECT result FROM jobs").fetchone()["result"] is None  # taken, not kept


def test_run_without_a_worker_fails_soon_and_leaves_nothing_queued(monkeypatch):
    jobs.use_workers()
    monkeypatch.setattr(jobs, "NO_WORKER_SECONDS", 0.3)
    with pytest.raises(ValueError, match="no research worker takes these jobs"):
        jobs.run({"files": [], "trials": [], "risk_free_rate": 0.05, "iv_price_mode": "mid"}, jobs.PRIORITY_PAGE)
    assert store.db().execute("SELECT status FROM jobs").fetchone()["status"] == "cancelled"


# ---------------------------------------------------------------- the runner's process stopped from outside

@pytest.fixture
def slow_batch(monkeypatch, tmp_path):
    """entry_backtest stood in for by a process that sleeps, so it can be stopped while it runs."""
    monkeypatch.setattr(strategies, "strategy_listing", lambda: {})
    monkeypatch.setattr(code_identity, "check_build", lambda *a: None)
    monkeypatch.setattr(strategies, "program", lambda *args: ["sleep", "30"])


def run_in_thread(name: str) -> tuple[threading.Thread, dict]:
    out: dict = {}

    def go() -> None:
        try:
            runner.run_trials([], [], name=name)
        except Exception as e:  # noqa: BLE001 - what the run ended with is the test's
            out["error"] = e
    t = threading.Thread(target=go)
    t.start()
    while name not in runner._running:
        time.sleep(0.01)
    return t, out


def test_a_cancelled_run_raises_cancelled(slow_batch):
    t, out = run_in_thread("page")
    assert runner.cancel("page")
    t.join(5)
    assert isinstance(out["error"], runner.Cancelled)


def test_a_run_the_kernel_killed_says_so(slow_batch):
    t, out = run_in_thread("killed")
    os.kill(runner._running["killed"].pid, signal.SIGKILL)  # as the OOM killer would
    t.join(5)
    assert isinstance(out["error"], ValueError) and "SIGKILL" in str(out["error"])
    assert "out of memory" in str(out["error"])


def test_the_day_cache_lets_the_files_read_longest_ago_go_first(tmp_path, monkeypatch):
    root = tmp_path / "cache" / "days"
    (root / "0123456789abcdef").mkdir(parents=True)  # a build's directory, as the cache was before
    (root / "0123456789abcdef" / "20250303-1.day").write_bytes(b"x")
    (root / "SPY").mkdir()
    for name, read in (("20250303-a.day", 300), ("20250303-SPY_20250303-b.chain", 200), ("20250304-c.day", 100)):
        (root / "SPY" / name).write_bytes(b"x" * 1000)
        os.utime(root / "SPY" / name, (time.time() - read, time.time()))
    monkeypatch.setattr(runner, "_trimmed", {})
    monkeypatch.setattr(runner.settings, "config", lambda: {"day_cache_gb": 1e-5})  # about 10 KB: room for all
    assert runner.day_cache_dir() == root
    runner.trim(root)
    assert [p.name for p in root.iterdir()] == ["SPY"]
    assert len(list((root / "SPY").iterdir())) == 3
    monkeypatch.setattr(runner.settings, "config", lambda: {"day_cache_gb": 2e-6})  # about 2.1 KB: two stay
    runner.trim(root)  # held to its budget a moment ago: not looked at again yet
    assert len(list((root / "SPY").iterdir())) == 3
    runner._trimmed.clear()
    runner.trim(root)
    assert sorted(p.name for p in (root / "SPY").iterdir()) == ["20250303-SPY_20250303-b.chain", "20250304-c.day"]
