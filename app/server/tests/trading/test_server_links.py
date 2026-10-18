"""The server's links that must not take it down: the numerics processes (a crash is one task's error, the next task
runs) and the trading engines' (one for each account, the live one's only when named; the log stream comes back after
it breaks; a status call that hangs does not stop the event loop; the Home view's state is read once for every page
watching, and not while none does)."""

from __future__ import annotations

import asyncio
import json
import threading
import time

import grpc
import pytest

from optra.infra import live, numerics

PRICING = {"spot": 100, "strike": 100, "rate": 0.03, "vol": 0.2, "maturity": 1}


@pytest.mark.skipif(not numerics.is_engine_available(), reason="libanalytics_c.so not built")
def test_a_numerics_process_that_dies_costs_one_task_not_the_server():
    assert numerics.run_engine_task("pricing", PRICING)["status"] == "ok"
    process = numerics._Process(numerics.library_build())
    process.proc.kill()
    process.proc.wait()
    with pytest.raises(numerics._Gone):
        process.ask({"task": "pricing", "threads": 1, "buffer": 4096}, json.dumps(PRICING).encode(), 5)
    numerics._idle[-1].proc.kill()  # one in the pool dies while idle: the next task starts another
    numerics._idle[-1].proc.wait()
    assert numerics.run_engine_task("pricing", PRICING)["status"] == "ok"


class _Broken(grpc.RpcError):
    def code(self) -> grpc.StatusCode:
        return grpc.StatusCode.UNAVAILABLE


class FakeEngine:
    """Gives two lines, breaks, gives two more; its status call takes a second."""

    def __init__(self) -> None:
        self.streams = 0

    def stream_logs(self):
        self.streams += 1
        yield f"line {self.streams}a"
        yield f"line {self.streams}b"
        raise _Broken()

    def get_status(self, timeout: float | None = None) -> dict:
        time.sleep(1.0)
        return {"status": "running", "connected": True, "detail": "engine: running; ib: on; md: off"}

    def close(self) -> None:
        pass


def test_the_log_stream_comes_back_and_a_slow_status_call_does_not_stop_the_loop(monkeypatch):
    monkeypatch.setattr(threading.Event, "wait", lambda self, t=None: self.is_set())  # no pause between tries

    engine = FakeEngine()

    async def scenario() -> tuple[float, list[str], dict]:
        link = live.Live(engine)
        await link.start()
        started, worst = time.monotonic(), 0.0
        while time.monotonic() - started < 1.5:  # the status call blocks a thread, never the loop
            t = time.monotonic()
            await asyncio.sleep(0.01)
            worst = max(worst, time.monotonic() - t)
        await link.stop()
        return worst, list(link.lines), link.status
    worst, lines, status = asyncio.run(scenario())
    assert worst < 0.2
    assert engine.streams > 1 and lines[-1].startswith("line ")  # the stream opened again each time it broke
    assert status["connected"]


def test_what_is_not_the_agents_needs_the_key(tmp_path, monkeypatch):
    from fastapi.testclient import TestClient

    from main import create_app
    from optra import settings
    from optra.api import jobs as jobs_api
    from optra.infra import access
    from optra.research import store

    monkeypatch.setattr(settings, "home", lambda: tmp_path / "home")
    monkeypatch.setattr(store, "db_path", lambda: tmp_path / "experiments.db")
    monkeypatch.setattr(store, "_db", None)
    client = TestClient(create_app(engine=False))
    key = {access.HEADER: access.key()}
    assert (tmp_path / "home" / "access.key").stat().st_mode & 0o777 == 0o600

    # The workers' side of the queue: a result handed in is a trial's, so only with the key.
    assert client.post("/api/jobs/lease", json={"worker": "w"}).status_code == 403
    assert client.post("/api/jobs/lease", json={"worker": "w"}, headers={access.HEADER: "forged"}).status_code == 403
    assert client.post("/api/jobs/1/result", params={"worker": "w"}, content=b"{}").status_code == 403
    # with the key, a result for a job not leased to it: refused, nothing written
    assert client.post("/api/jobs/1/result", params={"worker": "w"}, content=b'{"status":"error","error":"x"}',
                       headers=key).status_code == 409
    monkeypatch.setattr(jobs_api, "LONG_POLL_SECONDS", 0.0)
    assert client.post("/api/jobs/lease", json={"worker": "w"}, headers=key).json() == {"job": None}

    # The engine's controls: the person's. Without an engine the key gets as far as asking for it (503).
    for path in ("/api/risk/limits", "/api/strategies/x/start", "/api/gateway/connect", "/api/database/wipe"):
        assert client.post(path, json={}).status_code == 403, path
    assert client.post("/api/strategies/x/start", headers=key).status_code == 503
    assert client.get("/api/tools").json()["tools"][0]["who"] in ("agent", "person")


class StateEngine:
    """An engine whose strategies, holdings and risk the test changes; it notes when its strategies are read."""

    def __init__(self) -> None:
        self.strategies = [{"strategy_name": "a", "status": "running"}]
        self.reads: list[float] = []

    def get_status(self, timeout: float | None = None) -> dict:
        return {"status": "running", "connected": True, "detail": "engine: running; ib: on; md: on"}

    def list_strategies(self) -> list[dict]:
        self.reads.append(time.monotonic())
        return list(self.strategies)

    def get_strategy_holdings(self) -> dict:
        return {"holdings": {"a": {"summary": {"pnl": 1.5}}}}

    def get_risk(self) -> dict:
        raise _Broken()  # the state goes without it

    def stream_logs(self):
        return iter(())

    def close(self) -> None:
        pass


def test_the_home_state_is_pushed_to_every_page_from_one_reader(monkeypatch):
    from main import create_app

    monkeypatch.setattr(live, "POLL_SECONDS", 0.01)
    monkeypatch.setattr(live, "STATE_SECONDS", 0.05)
    monkeypatch.setattr(live, "UNCHANGED_SECONDS", 0.3)
    engine = StateEngine()
    app = create_app(engine=False)

    async def page(events: asyncio.Queue, gone: asyncio.Event) -> None:
        scope = {"type": "http", "asgi": {"version": "3.0", "spec_version": "2.3"}, "http_version": "1.1",
                 "method": "GET", "scheme": "http", "path": "/api/live/stream", "raw_path": b"/api/live/stream",
                 "root_path": "", "query_string": b"", "headers": [], "client": ("t", 1), "server": ("t", 80)}

        async def receive() -> dict:
            await gone.wait()
            return {"type": "http.disconnect"}

        async def send(message: dict) -> None:
            if message["type"] == "http.response.start":
                await events.put(dict(message["headers"])[b"content-type"])
            elif message.get("body"):
                await events.put(message["body"].decode())

        await app(scope, receive, send)

    async def scenario() -> None:
        link = live.Live(engine)
        monkeypatch.setitem(live._engines, "paper", link)
        await link.start()
        await asyncio.sleep(0.2)
        assert engine.reads == []  # no page watches: the engine is not asked
        pages = [(asyncio.Queue(), asyncio.Event()) for _ in range(2)]
        tasks = [asyncio.create_task(page(*p)) for p in pages]
        for events, _ in pages:
            assert (await asyncio.wait_for(events.get(), 2)).startswith(b"text/event-stream")
            first = json.loads((await asyncio.wait_for(events.get(), 2)).removeprefix("data: "))
            assert first == {"strategies": engine.strategies, "holdings": {"a": {"summary": {"pnl": 1.5}}},
                             "risk": None}
        engine.strategies = [{"strategy_name": "a", "status": "stopped"}]
        for events, _ in pages:  # the change, to both
            assert json.loads((await asyncio.wait_for(events.get(), 2)).removeprefix("data: "))["strategies"] == \
                engine.strategies
        for events, _ in pages:  # nothing changed since: said so
            assert await asyncio.wait_for(events.get(), 2) == "event: unchanged\ndata:\n\n"
        gaps = [b - a for a, b in zip(engine.reads, engine.reads[1:])]
        assert gaps and min(gaps) > 0.03  # one reader for both pages, at its own pace
        for _, gone in pages:
            gone.set()
        await asyncio.wait_for(asyncio.gather(*tasks), 2)
        await asyncio.sleep(0.2)
        read = len(engine.reads)
        await asyncio.sleep(0.3)
        assert len(engine.reads) == read and not link._watchers and link.state is None  # no page: not asked
        await link.stop()

    asyncio.run(scenario())


def test_each_account_has_its_engine_and_the_live_one_only_when_named(monkeypatch):
    from fastapi import HTTPException

    monkeypatch.delenv("LIVE_ACCOUNT_GRPC_TARGET", raising=False)
    monkeypatch.setenv("LIVE_GRPC_TARGET", "localhost:1")
    assert live.targets() == {"paper": "localhost:1"}
    monkeypatch.setenv("LIVE_ACCOUNT_GRPC_TARGET", "localhost:2")
    assert live.targets() == {"paper": "localhost:1", "live": "localhost:2"}

    paper = live.Live(object())
    monkeypatch.setattr(live, "_engines", {"paper": paper})
    assert live.client() is paper.engine and live.engine("paper") is paper.engine
    for account, code in (("live", 503), ("cash", 404)):  # not running here; no such account
        with pytest.raises(HTTPException) as refused:
            live.client(account)
        assert refused.value.status_code == code
