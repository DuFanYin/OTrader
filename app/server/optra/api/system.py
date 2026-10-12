"""System-level APIs: health, live restart, the engine's database (through the engine)."""

from __future__ import annotations

from typing import Any, Dict, Literal, Optional

from fastapi import APIRouter, Body, Query

from optra import settings
from optra.infra.state import AppState
from optra.infra import numerics
from optra.services.system import get_contracts_overview, get_orders_trades, wipe_database


router = APIRouter()


@router.get("/api/system/status")
def api_system_status() -> Dict[str, Any]:
    """Health: the server, the numerics library the lab computes with, the live engine."""
    return {
        "backend": {"status": "running"},
        "numerics": numerics.ENGINE_PROBLEM or "ok",
        "live": AppState.live.live_status,
        "demo": settings.demo(),
    }


@router.post("/api/system/restart_live")
def api_restart_live() -> Dict[str, Any]:
    """Soft restart: StopMarketData + Disconnect, then Connect + StartMarketData."""
    if AppState.live.live_client is None:
        return {"status": "error", "error": "live_client not initialized"}

    disc = AppState.live.live_client.disconnect_gateway()
    conn = AppState.live.live_client.connect_gateway()
    return {"status": "ok", "disconnect": disc, "connect": conn}


@router.get("/api/orders-trades")
def api_orders_trades(
    strategy: Optional[str] = Query(None, description="Filter by strategy_name"),
    record_type: Optional[str] = Query(
        None, description="Order, Trade, or omitted for both"
    ),
    limit: int = Query(100, ge=1, le=5000),
) -> Dict[str, Any]:
    """The orders and trades the engine saved, newest first (strategies + records)."""
    return get_orders_trades(strategy=strategy, record_type=record_type, limit=limit)


@router.get("/api/database/contracts")
def api_database_contracts() -> Dict[str, Any]:
    """Contract summary (equity + option totals and rows)."""
    return get_contracts_overview()


@router.post("/api/database/wipe")
def api_database_wipe(what: Literal["trading", "contracts"] = Body(..., embed=True)) -> Dict[str, Any]:
    """Empty the orders, trades and holdings, or the contracts: the engine does it."""
    return wipe_database(what)
