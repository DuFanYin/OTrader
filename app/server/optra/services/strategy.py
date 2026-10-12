"""Strategy service: list, add, init, start, stop, remove, delete, holdings."""

from fastapi import HTTPException

from optra.infra.state import AppState
from optra.schemas.engine import AddStrategyRequest, RestoreStrategyRequest


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
    def get_strategy_classes() -> dict:
        client = StrategyService._get_client_or_400()
        return {"classes": client.get_strategy_classes()}

    @staticmethod
    def get_strategy_class_defaults(strategy_class: str) -> dict:
        """Default settings for strategy class (as the class declares them)."""
        client = StrategyService._get_client_or_400()
        return {"settings": client.get_strategy_class_defaults(strategy_class)}

    @staticmethod
    def get_portfolios_meta() -> dict:
        client = StrategyService._get_client_or_400()
        return {"portfolios": client.get_portfolio_names()}

    @staticmethod
    def get_removed_strategies() -> dict:
        client = StrategyService._get_client_or_400()
        return {"removed_strategies": client.get_removed_strategies()}

    @staticmethod
    def add_strategy(req: AddStrategyRequest) -> dict:
        client = StrategyService._get_client_or_400()
        return client.add_strategy(
            req.strategy_class, req.portfolio_name, req.setting, req.strategy_name
        )

    @staticmethod
    def restore_strategy(req: RestoreStrategyRequest) -> dict:
        client = StrategyService._get_client_or_400()
        return client.restore_strategy(req.strategy_name)

    @staticmethod
    def init_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        return client.init_strategy(strategy_name)

    @staticmethod
    def start_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        return client.start_strategy(strategy_name)

    @staticmethod
    def stop_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        return client.stop_strategy(strategy_name)

    @staticmethod
    def remove_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        return client.remove_strategy(strategy_name)

    @staticmethod
    def delete_strategy(strategy_name: str) -> dict:
        client = StrategyService._get_client_or_400()
        return client.delete_strategy(strategy_name)

    @staticmethod
    def get_strategy_holdings() -> dict:
        client = StrategyService._get_client_or_400()
        return client.get_strategy_holdings()

