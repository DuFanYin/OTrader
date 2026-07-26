from __future__ import annotations

from typing import Any, Dict, List, Optional

from fastapi import HTTPException

from src.infra.db import DBError, fetch_contracts_summary, fetch_orders_trades_raw


def get_orders_trades_from_db(
    strategy: Optional[str],
    record_type: Optional[str],
    limit: int,
) -> Dict[str, Any]:
    """Historical orders/trades from DB; normalise record_type, shape for frontend."""
    rec_type = None
    if record_type is not None:
        rec_type = record_type.strip().capitalize()
        if rec_type not in {"Order", "Trade"}:
            raise HTTPException(status_code=400, detail="record_type must be Order or Trade")

    try:
        strategies, rows = fetch_orders_trades_raw(strategy=strategy, limit=limit)
    except DBError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    records: List[Dict[str, Any]] = []
    for (
        r_type,
        ts,
        strategy_name,
        id_value,
        symbol,
        direction,
        price,
        volume,
        traded,
        status,
    ) in rows:
        if rec_type and r_type != rec_type:
            continue
        records.append(
            {
                "record_type": r_type,
                "timestamp": ts,
                "strategy_name": strategy_name,
                "id": id_value,
                "symbol": symbol,
                "direction": direction,
                "price": float(price) if price is not None else None,
                "volume": float(volume) if volume is not None else None,
                "traded": float(traded) if traded is not None else None,
                "status": status,
            }
        )

    return {"strategies": strategies, "records": records}


def get_contracts_overview_from_db() -> Dict[str, Any]:
    """Contracts overview (equity + options); DBError → HTTPException."""
    try:
        return fetch_contracts_summary()
    except DBError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

