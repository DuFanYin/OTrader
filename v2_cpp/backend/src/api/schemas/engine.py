from __future__ import annotations

from typing import Any, Dict, Optional

from pydantic import BaseModel


class AddStrategyRequest(BaseModel):
    """Add live strategy request."""

    strategy_class: str
    portfolio_name: str
    setting: Optional[Dict[str, Any]] = None


class RestoreStrategyRequest(BaseModel):
    """Restore strategy (not supported in C++ live)."""

    strategy_name: str
    portfolio_name: Optional[str] = None
    setting: Optional[Dict[str, Any]] = None

