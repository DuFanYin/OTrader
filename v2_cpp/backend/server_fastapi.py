from __future__ import annotations

import asyncio
import sys
from collections.abc import AsyncGenerator
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, WebSocket
from fastapi.middleware.cors import CORSMiddleware



# Backend root for src.* imports
_BACKEND_ROOT = Path(__file__).resolve().parent
if str(_BACKEND_ROOT) not in sys.path:
    sys.path.insert(0, str(_BACKEND_ROOT))


@asynccontextmanager
async def _otrader_lifespan(app: FastAPI) -> AsyncGenerator[None, None]:
    """Start engine and associated background tasks when available."""
    from src.infra.state import AppState  # noqa: F401
    from src.infra.websockets import log_queue_processor
    from src.infra.live_client_lifespan import live_client_lifespan

    log_task: asyncio.Task | None = None
    try:
        try:
            AppState.main_loop = asyncio.get_running_loop()
        except RuntimeError:
            AppState.main_loop = asyncio.get_event_loop()
        log_task = asyncio.create_task(log_queue_processor())

        async with live_client_lifespan():
            yield
    except Exception as e:  # pragma: no cover - keep backtest-only mode working
        print(f"OTrader engine not loaded (backtest-only mode): {e}")
        yield
    finally:
        if log_task is not None:
            log_task.cancel()
            try:
                await log_task
            except asyncio.CancelledError:
                pass
        # C++ live engine lifecycle external


@asynccontextmanager
async def _lifespan(app: FastAPI) -> AsyncGenerator[None, None]:
    async with _otrader_lifespan(app):
        yield


def _include_routes(app: FastAPI) -> None:
    """Mount API routers and WebSockets."""
    try:
        from src.api.engine import router as engine_router
        from src.api.backtest import router as backtest_router
        from src.api.system import router as system_router
        from src.infra.websockets import handle_logs_websocket, handle_strategies_websocket

        app.include_router(engine_router)
        app.include_router(backtest_router)
        app.include_router(system_router)

        @app.websocket("/ws/logs")
        async def ws_logs(ws: WebSocket) -> None:
            await handle_logs_websocket(ws)

        @app.websocket("/ws/strategies")
        async def ws_strategies(ws: WebSocket) -> None:
            await handle_strategies_websocket(ws)
    except Exception as e:
        print(f"OTrader API not mounted (backtest-only): {e}")


app = FastAPI(title="FACTT Backend", version="0.1.0", lifespan=_lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=False,
    allow_methods=["GET", "POST", "PUT", "DELETE", "OPTIONS"],
    allow_headers=["*"],
)
_include_routes(app)




def run() -> None:
    import uvicorn

    uvicorn.run("backend.server_fastapi:app", host="0.0.0.0", port=8080, reload=True)


if __name__ == "__main__":
    run()

