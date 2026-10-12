"""Optra's server (uvicorn main:app, from app/server): one FastAPI app with one API. The trading routes
(optra.api: engine, backtest, session, system) reach the live engine through gRPC; the lab's page data is under
/api/lab; every tool is at its own route and at /api/tools; the page (app/web, built into
app/web/dist) is served at /."""

from __future__ import annotations

import asyncio
from collections.abc import AsyncGenerator
from contextlib import asynccontextmanager
from pathlib import Path

import grpc
from fastapi import FastAPI, Request, WebSocket
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import ValidationError

from optra.api import backtest, lab, session, system, tools
from optra.api import engine as engine_api
from optra.infra.live_client_lifespan import live_client_lifespan
from optra.infra.state import AppState
from optra.infra.websockets import handle_logs_websocket, handle_strategies_websocket, log_queue_processor
from optra.services import market_data
from optra.tools import ToolError

# The page, built by `npm run build` in app/web; served beside the API when built.
_WEB_DIST = Path(__file__).resolve().parents[1] / "web" / "dist"


@asynccontextmanager
async def _engine_lifespan() -> AsyncGenerator[None, None]:
    """The live engine's client and the log stream; without an engine the server still runs backtests and the lab."""
    log_task: asyncio.Task | None = None
    try:
        AppState.main_loop = asyncio.get_running_loop()
        log_task = asyncio.create_task(log_queue_processor())
        async with live_client_lifespan():
            yield
    except Exception as e:  # pragma: no cover - keep backtest-only mode working
        print(f"Optra engine not loaded (backtest-only mode): {e}")
        yield
    finally:
        if log_task is not None:
            log_task.cancel()
            try:
                await log_task
            except asyncio.CancelledError:
                pass


@asynccontextmanager
async def _no_engine() -> AsyncGenerator[None, None]:
    yield


# The engine's answer, as HTTP: a request it refuses (400, 404), one the engine's state does not
# allow now (409), the engine or IB not reachable (503), IB not answering in time (504).
_HTTP_STATUS = {
    grpc.StatusCode.INVALID_ARGUMENT: 400,
    grpc.StatusCode.OUT_OF_RANGE: 400,
    grpc.StatusCode.NOT_FOUND: 404,
    grpc.StatusCode.ALREADY_EXISTS: 409,
    grpc.StatusCode.FAILED_PRECONDITION: 409,
    grpc.StatusCode.ABORTED: 409,
    grpc.StatusCode.UNAVAILABLE: 503,
    grpc.StatusCode.DEADLINE_EXCEEDED: 504,
}


def create_app(engine: bool = True) -> FastAPI:
    """The server. engine=False leaves the live engine's client out (the tests: the lab and the tools only)."""

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncGenerator[None, None]:
        market_data.start_rate_refresh()  # the Treasury curve takes ~18 s: start it at once
        async with _engine_lifespan() if engine else _no_engine():
            yield
        await market_data.close()

    app = FastAPI(title="Optra", version="0.2.0", lifespan=lifespan)
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=False,
        allow_methods=["GET", "POST", "PUT", "DELETE", "OPTIONS"],
        allow_headers=["*"],
    )

    @app.exception_handler(grpc.RpcError)
    async def engine_error(_request: Request, exc: grpc.RpcError) -> JSONResponse:
        code = exc.code() if hasattr(exc, "code") else None
        detail = exc.details() if hasattr(exc, "details") else str(exc)
        return JSONResponse(status_code=_HTTP_STATUS.get(code, 502), content={"detail": detail})

    @app.exception_handler(ToolError)
    async def tool_error(_request: Request, exc: ToolError) -> JSONResponse:
        return JSONResponse({"detail": exc.message}, status_code=exc.status)

    @app.exception_handler(ValidationError)
    async def invalid_arguments(_request: Request, exc: ValidationError) -> JSONResponse:
        return JSONResponse({"detail": exc.errors(include_url=False, include_context=False)}, status_code=422)

    for module in (engine_api, backtest, system, session, lab, tools):
        app.include_router(module.router)

    @app.websocket("/ws/logs")
    async def ws_logs(ws: WebSocket) -> None:
        await handle_logs_websocket(ws)

    @app.websocket("/ws/strategies")
    async def ws_strategies(ws: WebSocket) -> None:
        await handle_strategies_websocket(ws)

    if _WEB_DIST.is_dir():  # after the API routes: it answers every other path
        app.mount("/", StaticFiles(directory=_WEB_DIST, html=True), name="web")
    return app


app = create_app()
