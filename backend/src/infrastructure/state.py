"""Application state management for FACTT backend."""

import asyncio
from queue import Queue
from typing import TYPE_CHECKING, Optional

from fastapi import WebSocket

if TYPE_CHECKING:
    from engines.engine_main import MainEngine


class AppState:
    """Global application state."""

    engine: Optional["MainEngine"] = None
    log_buffer: list[str] = []
    max_logs: int = 500
    log_clients: set[WebSocket] = set()
    strategy_clients: set[WebSocket] = set()
    log_queue: Queue = Queue()
    main_loop: asyncio.AbstractEventLoop | None = None

    # Strategy event cache for polling
    strategy_updates: list[dict] = []
    max_strategy_updates: int = 100
