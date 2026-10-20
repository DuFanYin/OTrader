"""The trading engines as the server sees them, one for each account (ACCOUNTS: the paper account's, at
LIVE_GRPC_TARGET; the live account's, at LIVE_ACCOUNT_GRPC_TARGET, only when it is set): each one's gRPC client, its
status polled every POLL_SECONDS off the event loop, its log lines streamed (again whenever the stream ends: the engine
restarted) into the recent lines and on to each page's WebSocket, and its state (strategies, holdings, risk) read every
STATE_SECONDS while a page watches it and sent to every watching page when it changes. Started and stopped with the
server (lifespan); client(account) is an engine's client for a route, engine its dependency (?account=, paper by
default)."""

from __future__ import annotations

import asyncio
import json
import os
import threading
from collections import deque
from collections.abc import AsyncGenerator, AsyncIterator
from contextlib import asynccontextmanager
from typing import Any

import grpc
from fastapi import HTTPException, Query, WebSocket, WebSocketDisconnect

from optra.infra.remote_client import EngineClient

POLL_SECONDS = 2.0
STATUS_DEADLINE = 2.0
RECENT_LINES = 500
CLIENT_QUEUE = 2000  # lines waiting for a slow page; past it the oldest go
STATE_SECONDS = 2.0
UNCHANGED_SECONDS = 15.0  # a watching page hears at least this often that its state still holds
ACCOUNTS = ("paper", "live")  # versions.accounts_of's


class Live:
    def __init__(self, engine: EngineClient) -> None:
        self.engine = engine
        self.status: dict[str, Any] = {"status": "unknown", "connected": False, "detail": "not asked yet"}
        self.lines: deque[str] = deque(maxlen=RECENT_LINES)
        self._pages: set[asyncio.Queue[str]] = set()
        self.state: str | None = None  # the state last sent to the watching pages, as JSON; None while none watch
        self._watchers: set[asyncio.Queue[str]] = set()
        self._watched = asyncio.Event()
        self._state_reader: asyncio.Task | None = None
        self._stop = threading.Event()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._poller: asyncio.Task | None = None
        self._streamer: threading.Thread | None = None

    async def start(self) -> None:
        self._loop = asyncio.get_running_loop()
        self._poller = asyncio.create_task(self._poll())
        self._state_reader = asyncio.create_task(self._read_state())
        self._streamer = threading.Thread(target=self._stream, name="engine-logs", daemon=True)
        self._streamer.start()

    async def stop(self) -> None:
        self._stop.set()
        for task in (self._poller, self._state_reader):
            if task is not None:
                task.cancel()
        self.engine.close()  # ends the log stream's call

    async def _poll(self) -> None:
        while True:
            try:
                self.status = await asyncio.to_thread(self.engine.get_status, STATUS_DEADLINE)
            except grpc.RpcError as e:
                self.status = {"status": "error", "connected": False, "detail": f"the engine does not answer: "
                                                                                f"{e.code().name}"}
            await asyncio.sleep(POLL_SECONDS)

    def _stream(self) -> None:
        """The engine's log stream, opened again after it ends, waiting longer each time it fails at once."""
        pause = 1.0
        while not self._stop.is_set():
            got = False
            try:
                for line in self.engine.stream_logs():
                    got = True
                    self._loop.call_soon_threadsafe(self._publish, line)
            except grpc.RpcError:
                pass
            pause = 1.0 if got else min(pause * 2, 30.0)
            self._stop.wait(pause)

    def _publish(self, line: str) -> None:
        self.lines.append(line)
        for page in self._pages:
            if page.full():
                page.get_nowait()
            page.put_nowait(line)

    async def serve_logs(self, ws: WebSocket) -> None:
        """A page's log: the recent lines, then each as it comes."""
        await ws.accept()
        page: asyncio.Queue[str] = asyncio.Queue(CLIENT_QUEUE)
        self._pages.add(page)
        receive = line = None
        try:
            for recent in list(self.lines)[-200:]:
                await ws.send_text(recent)
            receive = asyncio.create_task(ws.receive_text())  # raises WebSocketDisconnect when the page goes
            while True:
                line = asyncio.create_task(page.get())
                done, _ = await asyncio.wait({receive, line}, return_when=asyncio.FIRST_COMPLETED)
                if receive in done:
                    receive.result()  # a message from the page: nothing to do with it
                    receive = asyncio.create_task(ws.receive_text())
                if line in done:
                    await ws.send_text(line.result())
                else:
                    line.cancel()
        except (WebSocketDisconnect, RuntimeError):
            pass
        finally:
            self._pages.discard(page)
            for task in (receive, line):
                if task is not None:
                    task.cancel()

    def _state_now(self) -> str:
        """The strategies, their holdings and the risk state, as one JSON text; {"error"} when the engine does not
        answer (the risk state may be missing: null)."""
        try:
            state: dict[str, Any] = {"strategies": self.engine.list_strategies(),
                                     "holdings": self.engine.get_strategy_holdings()["holdings"]}
        except grpc.RpcError as e:
            return json.dumps({"error": f"the engine does not answer: {e.code().name}"})
        try:
            state["risk"] = self.engine.get_risk()
        except grpc.RpcError:
            state["risk"] = None
        return json.dumps(state, separators=(",", ":"))

    async def _read_state(self) -> None:
        """While a page watches: the engine's state read, and sent to each watching page when it changed."""
        while True:
            if not self._watchers:
                self.state = None
                self._watched.clear()
                await self._watched.wait()
            if self.status.get("status") != "running":
                self.state = None  # the page shows the engine down from its status
            elif (state := await asyncio.to_thread(self._state_now)) != self.state and self._watchers:
                self.state = state
                for page in self._watchers:
                    if page.full():
                        page.get_nowait()  # a page that has not taken the last one gets this one instead
                    page.put_nowait(state)
            await asyncio.sleep(STATE_SECONDS)

    async def watch_state(self) -> AsyncIterator[str]:
        """A page's state as Server-Sent Events: the state now, then each time it changes; an "unchanged" event
        when it has not for UNCHANGED_SECONDS."""
        page: asyncio.Queue[str] = asyncio.Queue(1)
        self._watchers.add(page)
        self._watched.set()
        try:
            if self.state is not None:
                yield f"data: {self.state}\n\n"
            while True:
                try:
                    yield f"data: {await asyncio.wait_for(page.get(), UNCHANGED_SECONDS)}\n\n"
                except TimeoutError:  # a comment while there is no state (the engine not running), to keep the line
                    yield "event: unchanged\ndata:\n\n" if self.state is not None else ": no state\n\n"
        finally:
            self._watchers.discard(page)


_engines: dict[str, Live] = {}


def targets() -> dict[str, str]:
    """Each account's engine: the paper one always, the live one when LIVE_ACCOUNT_GRPC_TARGET names it."""
    out = {"paper": os.getenv("LIVE_GRPC_TARGET", "localhost:50051")}
    if live_target := os.getenv("LIVE_ACCOUNT_GRPC_TARGET"):
        out["live"] = live_target
    return out


def client(account: str = "paper") -> EngineClient:
    """The account's engine's client; 404 for no such account, 503 when the server runs without its engine."""
    if account not in ACCOUNTS:
        raise HTTPException(404, f"no account {account}: {', '.join(ACCOUNTS)}")
    if account not in _engines:
        raise HTTPException(503, f"the server runs without the {account} account's engine")
    return _engines[account].engine


def engine(account: str = Query("paper", description="Whose engine: the paper account's or the live one's")) \
        -> EngineClient:
    """A route's engine (a dependency)."""
    return client(account)


def link(account: str = "paper") -> Live | None:
    return _engines.get(account)


def links() -> dict[str, Live]:
    return dict(_engines)


def flag(name: str, account: str = "paper") -> dict[str, Any]:
    """ib or md (market data) from the status the engine last gave ("engine: running; ib: on; md: off")."""
    found = _engines.get(account)
    status = found.status if found is not None else {"status": "stopped", "detail": ""}
    detail = status.get("detail") or ""
    on = f"{name}: on" in detail
    return {"status": "running" if on else "stopped", "connected": on, "detail": detail}


@asynccontextmanager
async def lifespan() -> AsyncGenerator[None, None]:
    for account, target in targets().items():
        _engines[account] = Live(EngineClient(target))
        await _engines[account].start()
    try:
        yield
    finally:
        for each in _engines.values():
            await each.stop()
        _engines.clear()
