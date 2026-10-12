"""WebSocket handlers and log bridging for Optra backend."""

import asyncio

from fastapi import WebSocket, WebSocketDisconnect

from optra.infra.state import AppState


async def log_queue_processor() -> None:
    """Send log queue to WebSocket clients."""
    while True:
        try:
            try:
                log_line = AppState.live.log_queue.get_nowait()
            except Exception:
                await asyncio.sleep(0.1)
                continue
            for ws in list(AppState.ws.log_clients):
                try:
                    await ws.send_text(log_line)
                except Exception:
                    AppState.ws.log_clients.discard(ws)
        except Exception:
            pass
        await asyncio.sleep(0.1)


async def handle_logs_websocket(ws: WebSocket) -> None:
    """Logs WebSocket: backlog + stream."""
    await ws.accept()
    AppState.ws.log_clients.add(ws)
    for line in AppState.live.log_buffer[-200:]:
        try:
            await ws.send_text(line)
        except Exception:
            break
    try:
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        AppState.ws.log_clients.discard(ws)


async def handle_strategies_websocket(ws: WebSocket) -> None:
    """Strategies WebSocket (live C++; connection kept for future push)."""
    await ws.accept()
    AppState.ws.strategy_clients.add(ws)
    try:
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        AppState.ws.strategy_clients.discard(ws)

