"""App state: gateway, strategies, logs (LiveEngineState, BacktestState, WebsocketState)."""

from __future__ import annotations

import asyncio
from queue import Queue
from threading import Event
from typing import TYPE_CHECKING, Any, Dict, Optional, Set

from fastapi import WebSocket

if TYPE_CHECKING:
    # Type hints only
    from src.infra.remote_client import EngineClient  # pragma: no cover


class LiveEngineState:
    """Live C++ gRPC engine and logs."""

    #: gRPC client
    live_client: Optional["EngineClient"] = None
    #: Status cache (heartbeat)
    live_status: Dict[str, Any] = {
        "status": "unknown",
        "connected": False,
        "detail": "",
    }

    #: Log buffer
    log_buffer: list[str] = []
    max_logs: int = 500

    #: Log stream queue
    log_queue: Queue[str] = Queue()

    #: Strategy updates
    strategy_updates: list[dict] = []
    max_strategy_updates: int = 100

    #: Log task handle
    live_log_task: asyncio.Task | None = None

    #: Log stream lifecycle
    log_stop_event: Event | None = None
    log_stream_alive: bool = False


class BacktestState:
    """State related to C++ backtest process management."""

    backtest_proc: Any | None = None


class WebsocketState:
    """State for active WebSocket connections."""

    log_clients: Set[WebSocket] = set()
    strategy_clients: Set[WebSocket] = set()


class AppState:
    """Global state: .live, .backtest, .ws."""

    # Sub-states
    live: LiveEngineState = LiveEngineState()
    backtest: BacktestState = BacktestState()
    ws: WebsocketState = WebsocketState()

    # Main event loop
    main_loop: asyncio.AbstractEventLoop | None = None

