"""WebSocket handlers for real-time communication."""

import asyncio
import json
from datetime import datetime

from fastapi import WebSocket, WebSocketDisconnect

from .state import AppState


def handle_loguru_log(record: dict) -> None:
    """Handle direct loguru log records for WebSocket broadcasting."""
    try:
        # Extract log data from loguru record
        level_name = record["level"].name
        timestamp = datetime.fromtimestamp(record["time"].timestamp()).strftime("%d/%m %H:%M:%S")
        gateway_name = record["extra"].get("gateway_name", "Unknown")
        message = record["message"]

        # Format using LogEngine's format
        formatted_msg = f"{timestamp} | {level_name:<7} | {gateway_name:<10} | {message}"

        # Add to log buffer
        AppState.log_buffer.append(formatted_msg)
        if len(AppState.log_buffer) > AppState.max_logs:
            AppState.log_buffer = AppState.log_buffer[-AppState.max_logs :]

        # Send to WebSocket clients via queue
        AppState.log_queue.put(formatted_msg)
    except Exception as e:
        # Log error but don't raise to avoid breaking log processing
        print(f"Error handling loguru log: {e}")


async def log_queue_processor() -> None:
    """Background task to process log queue and send to WebSocket clients."""
    while True:
        try:
            # Get log from queue (non-blocking)
            try:
                log_line = AppState.log_queue.get_nowait()
            except Exception:
                # No logs available, sleep briefly
                await asyncio.sleep(0.1)
                continue

            # Send to all WebSocket clients
            for ws in list(AppState.log_clients):
                try:
                    await ws.send_text(log_line)
                except Exception:
                    AppState.log_clients.discard(ws)
        except Exception:
            pass
            await asyncio.sleep(0.1)


async def handle_logs_websocket(ws: WebSocket) -> None:
    """Handle logs WebSocket connection."""
    await ws.accept()
    AppState.log_clients.add(ws)

    # Send recent history first
    for line in AppState.log_buffer[-200:]:
        try:
            await ws.send_text(line)
        except Exception:
            break

    try:
        while True:
            # Keep connection alive; we don't expect messages from client
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        AppState.log_clients.discard(ws)


async def handle_strategies_websocket(ws: WebSocket) -> None:
    """Handle strategies WebSocket connection."""
    await ws.accept()
    # Send current snapshot for all strategies
    try:
        if AppState.engine is not None:
            for s in AppState.engine.option_strategy_engine.strategies.values():
                variables: dict = s.get_variables()
                payload: dict = {
                    "strategy_name": s.strategy_name,
                    "class_name": s.__class__.__name__,
                    "author": s.author,
                    "parameters": s.get_parameters(),
                    "variables": variables,
                }
                if s.holding:
                    summary = s.holding.summary
                    payload["variables"]["holding"] = {
                        "pnl": summary.pnl,
                        "current_value": summary.current_value,
                        "total_cost": summary.total_cost,
                        "delta": summary.delta,
                        "gamma": summary.gamma,
                        "theta": summary.theta,
                        "vega": summary.vega,
                    }
                await ws.send_text(json.dumps(payload))
    except Exception as e:
        # Log error but don't raise to avoid breaking WebSocket connection
        print(f"Error sending strategy data: {e}")
    AppState.strategy_clients.add(ws)
    try:
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        AppState.strategy_clients.discard(ws)
