"""Engine event listeners for WebSocket and HTTP clients."""

import json
from typing import TYPE_CHECKING

from engines.engine_event import Event
from utilities.event import EVENT_LOG, EVENT_PORTFOLIO_STRATEGY

from .state import AppState

if TYPE_CHECKING:
    from engines.engine_main import MainEngine


def attach_engine_listeners(engine: "MainEngine") -> None:
    """Register engine event handlers to feed HTTP/WS clients."""

    # WebSocket logging is already setup in main.py before engine initialization

    def _on_log(event: Event) -> None:
        # Skip engine event logs since we're using loguru WebSocket handler
        # This prevents duplicate logs
        pass

    def _on_strategy(event: Event) -> None:
        # Cache strategy update for polling
        AppState.strategy_updates.append(event.data)
        if len(AppState.strategy_updates) > AppState.max_strategy_updates:
            AppState.strategy_updates = AppState.strategy_updates[-AppState.max_strategy_updates :]

        # Send to WebSocket clients
        payload = json.dumps(event.data)
        for ws in list(AppState.strategy_clients):
            try:
                import anyio

                anyio.from_thread.run(ws.send_text, payload)
            except Exception:
                AppState.strategy_clients.discard(ws)

    # Register our log handler BEFORE the engine's log handler
    engine.event_engine.register(EVENT_LOG, _on_log)
    engine.event_engine.register(EVENT_PORTFOLIO_STRATEGY, _on_strategy)
