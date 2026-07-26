"""Strategy service: list, add, init, start, stop, remove, delete, holdings."""

import logging

from fastapi import HTTPException

from src.infra.state import AppState
from src.api.schemas.engine import AddStrategyRequest, RestoreStrategyRequest


class StrategyService:
    """Strategy-related operations."""

    @staticmethod
    def _get_client_or_400():
        client = AppState.live.live_client
        if client is None:
            raise HTTPException(status_code=400, detail="live engine (gRPC) not started")
        return client

    @staticmethod
    def list_strategies() -> dict:
        client = StrategyService._get_client_or_400()
        strategies = client.list_strategies()
        return {"strategies": strategies}

    @staticmethod
    def get_strategy_updates() -> dict:
        return {"updates": AppState.live.strategy_updates.copy()}

    @staticmethod
    def clear_strategy_updates() -> dict:
        AppState.live.strategy_updates.clear()
        return {"status": "ok", "message": "Strategy updates cleared"}

    @staticmethod
    def get_strategy_classes() -> dict:
        client = StrategyService._get_client_or_400()
        try:
            return {"classes": client.get_strategy_classes()}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def get_strategy_class_defaults(strategy_class: str) -> dict:
        """Default settings for strategy class (strategy_config.json)."""
        client = StrategyService._get_client_or_400()
        try:
            settings = client.get_strategy_class_defaults(strategy_class)
            return {"settings": settings}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def get_portfolios_meta() -> dict:
        client = StrategyService._get_client_or_400()
        try:
            return {"portfolios": client.get_portfolios_meta()}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def get_removed_strategies() -> dict:
        client = StrategyService._get_client_or_400()
        return {"removed_strategies": client.get_removed_strategies()}

    @staticmethod
    def add_strategy(req: AddStrategyRequest) -> dict:
        client = StrategyService._get_client_or_400()
        try:
            return client.add_strategy(req.strategy_class, req.portfolio_name, req.setting)
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def restore_strategy(req: RestoreStrategyRequest) -> dict:
        raise HTTPException(status_code=400, detail="RestoreStrategy is not supported in C++ live mode")

    @staticmethod
    def init_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        try:
            return client.init_strategy(strategy_name)
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def start_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        try:
            return client.start_strategy(strategy_name)
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def stop_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        try:
            return client.stop_strategy(strategy_name)
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def remove_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        try:
            return client.remove_strategy(strategy_name)
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def delete_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        try:
            return client.delete_strategy(strategy_name)
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def get_strategy_holdings() -> dict:
        client = StrategyService._get_client_or_400()
        return client.get_strategy_holdings()

