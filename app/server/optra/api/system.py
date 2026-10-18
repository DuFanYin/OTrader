"""System-level APIs: health, live restart, the engine's database (through the engine)."""

from __future__ import annotations

from typing import Any, Dict, Literal, Optional

from fastapi import APIRouter, Body, Depends, Query

from optra import settings
from optra.infra import access, live, numerics
from optra.infra.remote_client import EngineClient
from optra.services.system import get_contracts_overview, get_orders_fills, wipe_database


router = APIRouter()
ANYONE = [Depends(access.anyone)]


@router.get("/api/system/status", dependencies=ANYONE)
def api_system_status() -> Dict[str, Any]:
    """The server, the numerics library the lab computes with, the paper account's engine as last polled (live), and
    every account's (engines)."""
    link = live.link("paper")
    return {
        "backend": {"status": "running"},
        "numerics": numerics.engine_problem() or "ok",
        "live": link.status if link else {"status": "stopped", "connected": False, "detail": "no live engine"},
        "engines": {account: each.status for account, each in live.links().items()},
        "demo": settings.demo(),
    }


@router.get("/api/health", dependencies=ANYONE)
async def api_health() -> Dict[str, str]:
    """The server's event loop answers: nothing else is asked (the watchdog's and a load balancer's check)."""
    return {"status": "ok"}


@router.post("/api/system/restart_live", dependencies=[Depends(access.person_only)])
def api_restart_live(engine: EngineClient = Depends(live.engine)) -> Dict[str, Any]:
    """Reconnect IB: market data stopped and the gateway disconnected, then connected again."""
    return {"status": "ok", "disconnect": engine.disconnect_gateway(), "connect": engine.connect_gateway()}


@router.get("/api/orders-fills", dependencies=ANYONE)
def api_orders_fills(
    strategy: Optional[str] = Query(None, description="Filter by strategy_name"),
    record_type: Optional[str] = Query(
        None, description="Order, Fill, or omitted for both"
    ),
    limit: int = Query(100, ge=1, le=5000),
    engine: EngineClient = Depends(live.engine),
) -> Dict[str, Any]:
    """The orders and fills the engine saved, newest first (strategies + records)."""
    return get_orders_fills(engine, strategy=strategy, record_type=record_type, limit=limit)


@router.get("/api/database/contracts", dependencies=ANYONE)
def api_database_contracts(engine: EngineClient = Depends(live.engine)) -> Dict[str, Any]:
    """Contract summary (equity + option totals and rows)."""
    return get_contracts_overview(engine)


@router.post("/api/database/wipe", dependencies=[Depends(access.person_only)])
def api_database_wipe(what: Literal["trading", "contracts"] = Body(..., embed=True),
                      engine: EngineClient = Depends(live.engine)) -> Dict[str, Any]:
    """Empty the orders, fills and holdings, or the contracts: the engine does it."""
    return wipe_database(engine, what)
