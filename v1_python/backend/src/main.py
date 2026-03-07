"""Main FastAPI application."""

import asyncio
import sys
from collections.abc import AsyncGenerator
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, WebSocket
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles

# Add project root to path for local imports
project_root = Path(__file__).resolve().parents[2]
if str(project_root) not in sys.path:
    sys.path.insert(0, str(project_root))

# Local imports after path modification
from .api import engine_api, static_api  # noqa: E402
from .infrastructure.state import AppState  # noqa: E402
from .infrastructure.websockets import handle_logs_websocket, handle_strategies_websocket, log_queue_processor  # noqa: E402


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncGenerator[None, None]:
    """Application lifespan manager."""
    # Store the main event loop
    AppState.main_loop = asyncio.get_event_loop()

    # Start background task for log processing
    log_task = asyncio.create_task(log_queue_processor())

    # Setup WebSocket logging BEFORE engine initialization to capture all logs
    from engines.engine_log import add_websocket_sink, set_websocket_log_handler

    from .infrastructure.websockets import handle_loguru_log

    set_websocket_log_handler(handle_loguru_log)
    add_websocket_sink()

    # Auto-create main engine on startup
    try:
        if AppState.engine is None:
            from engines.engine_event import EventEngine
            from engines.engine_main import MainEngine

            AppState.engine = MainEngine(EventEngine())
            from .infrastructure.events import attach_engine_listeners

            # Attach engine listeners (WebSocket logging already setup above)
            attach_engine_listeners(AppState.engine)

        yield
    finally:
        # Cancel background task
        log_task.cancel()
        try:
            await log_task
        except asyncio.CancelledError:
            pass

        if AppState.engine is not None:
            try:
                AppState.engine.close()
            except Exception as e:
                # Log error but don't raise to avoid breaking shutdown
                print(f"Error closing engine: {e}")


def create_app() -> FastAPI:
    """Create and configure the FastAPI application."""
    app = FastAPI(title="OTrader Backend", version="0.1.0", lifespan=lifespan)

    # Add CORS middleware
    app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_credentials=True, allow_methods=["*"], allow_headers=["*"])

    # Include unified engine router and static routes
    app.include_router(engine_api.router)
    app.include_router(static_api.router)

    # WebSocket routes
    @app.websocket("/ws/logs")
    async def ws_logs(ws: WebSocket) -> None:
        await handle_logs_websocket(ws)

    @app.websocket("/ws/strategies")
    async def ws_strategies(ws: WebSocket) -> None:
        await handle_strategies_websocket(ws)

    # Mount frontend static files (must be after all API routes)
    frontend_path = Path(__file__).resolve().parents[2] / "frontend"
    if frontend_path.exists():
        app.mount("/static", StaticFiles(directory=str(frontend_path)), name="static")
        app.mount("/", StaticFiles(directory=str(frontend_path), html=True), name="frontend")

    return app


# Create the app instance
app = create_app()
