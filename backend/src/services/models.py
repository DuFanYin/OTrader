"""Pydantic models for API requests and responses."""

from typing import Any

from pydantic import BaseModel


class AddStrategyRequest(BaseModel):
    """Request model for adding a new strategy."""

    strategy_class: str
    portfolio_name: str
    setting: dict[str, Any] = {}


class RestoreStrategyRequest(BaseModel):
    """Request model for restoring a strategy."""

    strategy_name: str
