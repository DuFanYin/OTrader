from __future__ import annotations

from typing import Any, Dict, Optional

from pydantic import BaseModel


class AddStrategyRequest(BaseModel):
    """Add live strategy request."""

    strategy_class: str
    portfolio_name: str
    setting: Optional[Dict[str, Any]] = None
    strategy_name: Optional[str] = None  # instance name; empty = <strategy_class>_<portfolio_name>


class RestoreStrategyRequest(BaseModel):
    """Restore a removed strategy as it was: its portfolio and setting are the engine's record of
    them, so portfolio_name and setting are ignored."""

    strategy_name: str
    portfolio_name: Optional[str] = None
    setting: Optional[Dict[str, Any]] = None

