"""WebSocket handlers and log bridging for OTrader backend."""

import asyncio
import json
from datetime import datetime

from fastapi import WebSocket, WebSocketDisconnect

from src.infra.state import AppState


def handle_loguru_log(record: dict) -> None:
    """Loguru records → buffer + queue for WebSocket."""
    try:
        level_name = record["level"].name
        timestamp = datetime.fromtimestamp(record["time"].timestamp()).strftime("%d/%m %H:%M:%S")
        gateway_name = record["extra"].get("gateway_name", "Unknown")
        message = record["message"]
        formatted_msg = f"{timestamp} | {level_name:<7} | {gateway_name:<10} | {message}"
        AppState.live.log_buffer.append(formatted_msg)
        if len(AppState.live.log_buffer) > AppState.live.max_logs:
            AppState.live.log_buffer = AppState.live.log_buffer[-AppState.live.max_logs :]
        AppState.live.log_queue.put(formatted_msg)
    except Exception as e:
        print(f"Error handling loguru log: {e}")


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

