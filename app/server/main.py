"""Optra's server (uvicorn main:app, from app/server): one FastAPI app with one API. The trading routes
(optra.api: engine, backtest, session, system) reach the live engine through gRPC; the lab's page data is under
/api/lab; every tool is at its own route and at /api/tools; the page (app/web, built into
app/web/dist) is served at /."""

from __future__ import annotations

import asyncio
from collections.abc import AsyncGenerator
from contextlib import AsyncExitStack, asynccontextmanager
from pathlib import Path

import grpc
from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import ValidationError

from optra.api import backtest, jobs as jobs_api, lab, session, system, tools
from optra.api import engine as engine_api
from optra.infra import access, live, numerics, watchdog
from optra.research import jobs
from optra.services import market_data
from optra.tools import ToolError

# The page, built by `npm run build` in app/web; served beside the API when built.
_WEB_DIST = Path(__file__).resolve().parents[1] / "web" / "dist"


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


class _Who:
    """A request with the key (X-Optra-Key) is the person's or the system's; any other, the agent's
    (infra/access.py)."""

    def __init__(self, app) -> None:
        self.app = app

    async def __call__(self, scope, receive, send) -> None:
        if scope["type"] in ("http", "websocket"):
            given = dict(scope["headers"]).get(access.HEADER.encode())
            if given and access.holds_key(given.decode(errors="replace")):
                with access.system():
                    return await self.app(scope, receive, send)
        await self.app(scope, receive, send)


def create_app(engine: bool = True) -> FastAPI:
    """The server. engine=False leaves the live engine's link out (the tests: the lab and the tools only)."""

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncGenerator[None, None]:
        jobs.use_workers()  # backtests are jobs for the research workers, never run in this process
        market_data.start_rate_refresh()  # the Treasury curve takes ~18 s: start it at once
        beat = asyncio.create_task(watchdog.run())
        async with AsyncExitStack() as stack:
            if engine:
                await stack.enter_async_context(live.lifespan())
            yield
        beat.cancel()
        await market_data.close()
        numerics.shutdown()
        jobs.use_workers(False)

    app = FastAPI(title="Optra", version="0.2.0", lifespan=lifespan)
    app.add_middleware(_Who)
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

    @app.exception_handler(access.NotTheAgents)
    async def not_the_agents(_request: Request, exc: access.NotTheAgents) -> JSONResponse:
        return JSONResponse({"detail": str(exc)}, status_code=403)

    @app.exception_handler(ToolError)
    async def tool_error(_request: Request, exc: ToolError) -> JSONResponse:
        return JSONResponse({"detail": exc.message}, status_code=exc.status)

    @app.exception_handler(numerics.EngineError)
    async def numerics_error(_request: Request, exc: numerics.EngineError) -> JSONResponse:
        return JSONResponse({"detail": str(exc)}, status_code=exc.status)

    @app.exception_handler(ValidationError)
    async def invalid_arguments(_request: Request, exc: ValidationError) -> JSONResponse:
        return JSONResponse({"detail": exc.errors(include_url=False, include_context=False)}, status_code=422)

    routers = [m.router for m in (engine_api, backtest, jobs_api, system, session, lab, tools)]
    if open_by_default := access.undeclared([r for router in routers for r in router.routes]):
        raise RuntimeError(f"routes that do not say whose they are (access.WHO): {', '.join(open_by_default)}")
    for router in routers:
        app.include_router(router)

    if _WEB_DIST.is_dir():  # after the API routes: it answers every other path
        app.mount("/", StaticFiles(directory=_WEB_DIST, html=True), name="web")
    return app


app = create_app()
