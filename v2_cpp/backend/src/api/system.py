"""System-level APIs: health, live restart, DB views."""

from __future__ import annotations

from typing import Any, Dict, Optional

from fastapi import APIRouter, Query

from src.infra.state import AppState
from src.services.system import (
    get_contracts_overview_from_db,
    get_orders_trades_from_db,
)


router = APIRouter()


@router.get("/api/system/status")
def api_system_status() -> Dict[str, Any]:
    """Health: backend + live engine status."""
    return {
        "backend": {"status": "running"},
        "live": AppState.live.live_status,
    }


@router.post("/api/system/restart_live")
def api_restart_live() -> Dict[str, Any]:
    """Soft restart: StopMarketData + Disconnect, then Connect + StartMarketData."""
    if AppState.live.live_client is None:
        return {"status": "error", "error": "live_client not initialized"}

    disc = AppState.live.live_client.disconnect_gateway()
    conn = AppState.live.live_client.connect_gateway()
    return {"status": "ok", "disconnect": disc, "connect": conn}


@router.get("/api/orders-trades/db")
def api_orders_trades_db(
    strategy: Optional[str] = Query(None, description="Filter by strategy_name"),
    record_type: Optional[str] = Query(
        None, description="Order, Trade, or omitted for both"
    ),
    limit: int = Query(100, ge=1, le=5000),
) -> Dict[str, Any]:
    """Historical orders/trades from PostgreSQL (strategies + records)."""
    return get_orders_trades_from_db(
        strategy=strategy,
        record_type=record_type,
        limit=limit,
    )


@router.get("/api/database/contracts")
def api_database_contracts() -> Dict[str, Any]:
    """Contract summary (equity + option totals and rows)."""
    return get_contracts_overview_from_db()

