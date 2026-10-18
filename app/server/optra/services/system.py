"""The engine's database as the UI shows it: orders and trades, contracts, emptying. All of it through
the engine (gRPC): the backend never opens the engine's file."""

from __future__ import annotations

from collections import defaultdict
from datetime import date
from typing import Any, Dict, Optional

from fastapi import HTTPException

from optra.infra.remote_client import EngineClient


def get_orders_fills(client: EngineClient, strategy: Optional[str], record_type: Optional[str],
                     limit: int) -> Dict[str, Any]:
    """The latest `limit` orders and fills (or only one kind), newest first, and the strategy
    names to filter by."""
    rec_type = None
    if record_type is not None:
        rec_type = record_type.strip().capitalize()
        if rec_type not in {"Order", "Fill"}:
            raise HTTPException(status_code=400, detail="record_type must be Order or Fill")
    data = client.get_orders_and_fills(strategy, limit)
    records = [r for r in data["records"] if rec_type is None or r["record_type"] == rec_type]
    return {"strategies": data["strategies"], "records": records[:limit]}


def get_contracts_overview(client: EngineClient) -> Dict[str, Any]:
    """The contracts saved: the equities (totals, first 50) and the options by chain (totals, the
    100 nearest chains with their calls, puts and strike range)."""
    contracts = client.list_saved_contracts()
    equities = sorted((c for c in contracts if not c["option_type"]), key=lambda c: c["symbol"])
    equity_rows = [
        {k: c[k] for k in ("symbol", "product", "multiplier", "price_tick")}
        for c in equities[:50]
    ]

    # One chain per symbol prefix (ROOT-EXPIRY-...): its expiry, calls, puts and strike range.
    chains: Dict[str, Dict[str, Any]] = defaultdict(
        lambda: {"calls": 0, "puts": 0, "strikes": [], "expiry": None, "underlying": None}
    )
    option_total = 0
    for c in contracts:
        if not c["option_type"]:
            continue
        option_total += 1
        chain = chains["_".join(c["symbol"].split("-")[:2])]
        chain["calls" if c["option_type"] == "CALL" else "puts"] += 1
        chain["strikes"].append(c["option_strike"])
        expiry = c["option_expiry"] or None
        if expiry and (chain["expiry"] is None or expiry < chain["expiry"]):
            chain["expiry"] = expiry
        chain["underlying"] = chain["underlying"] or c["option_underlying"] or None

    today = date.today()
    option_rows = [
        {
            "chain": name,
            "dte": max(0, (date.fromisoformat(c["expiry"]) - today).days) if c["expiry"] else None,
            "calls": c["calls"],
            "puts": c["puts"],
            "strike_min": min(c["strikes"], default=None),
            "strike_max": max(c["strikes"], default=None),
            "underlying": c["underlying"],
        }
        for name, c in sorted(chains.items(), key=lambda kv: (kv[1]["expiry"] or "", kv[0]))
    ][:100]

    return {
        "equity": {"total": len(equities), "rows": equity_rows},
        "option": {"total": option_total, "chains": len(chains), "rows": option_rows},
    }


def wipe_database(client: EngineClient, what: str) -> Dict[str, Any]:
    """Empty the trading record or the contracts: the engine does it, or refuses (409) while a
    strategy holds positions."""
    return {"status": "ok", "emptied": client.wipe_database(what)}
