"""Unified engine API router: gateway, strategies, holdings, orders-trades, logs."""

from fastapi import APIRouter, Query

from src.api.schemas.engine import AddStrategyRequest, RestoreStrategyRequest
from src.services.main import MainService
from src.services.strategy import StrategyService


router = APIRouter()


@router.post("/api/gateway/connect")
def connect_gateway() -> dict:
    return MainService.connect_gateway()


@router.post("/api/gateway/disconnect")
def disconnect_gateway() -> dict:
    return MainService.disconnect_gateway()


@router.get("/api/gateway/status")
def get_gateway_status() -> dict:
    return MainService.get_gateway_status()


@router.get("/api/market/status")
def get_market_status() -> dict:
    return MainService.get_market_status()


@router.post("/api/market/start")
def start_market() -> dict:
    return MainService.start_market_data()


@router.post("/api/market/stop")
def stop_market() -> dict:
    return MainService.stop_market_data()


@router.get("/api/strategies")
def list_strategies() -> dict:
    return StrategyService.list_strategies()


@router.get("/api/strategies/updates")
def get_strategy_updates() -> dict:
    return StrategyService.get_strategy_updates()


@router.get("/api/strategies/updates/clear")
def clear_strategy_updates() -> dict:
    return StrategyService.clear_strategy_updates()


@router.get("/api/strategies/meta/strategy-classes")
def get_strategy_classes() -> dict:
    return StrategyService.get_strategy_classes()


@router.get("/api/strategies/meta/settings")
def get_strategy_class_settings(strategy_class: str = Query(..., alias="class")) -> dict:
    """Default settings for strategy class (strategy_config.json)."""
    return StrategyService.get_strategy_class_defaults(strategy_class)


@router.get("/api/strategies/meta/portfolios")
def get_portfolios() -> dict:
    return StrategyService.get_portfolios_meta()


@router.get("/api/strategies/meta/removed-strategies")
def get_removed_strategies() -> dict:
    return StrategyService.get_removed_strategies()


@router.post("/api/strategies")
def add_strategy(req: AddStrategyRequest) -> dict:
    return StrategyService.add_strategy(req)


@router.post("/api/strategies/restore")
def restore_strategy(req: RestoreStrategyRequest) -> dict:
    return StrategyService.restore_strategy(req)


@router.post("/api/strategies/{strategy_name}/init")
def init_strategy(strategy_name: str) -> dict:
    return StrategyService.init_strategy(strategy_name)


@router.post("/api/strategies/{strategy_name}/start")
def start_strategy(strategy_name: str) -> dict:
    return StrategyService.start_strategy(strategy_name)


@router.post("/api/strategies/{strategy_name}/stop")
def stop_strategy(strategy_name: str) -> dict:
    return StrategyService.stop_strategy(strategy_name)


@router.delete("/api/strategies/{strategy_name}/remove")
def remove_strategy(strategy_name: str) -> dict:
    return StrategyService.remove_strategy(strategy_name)


@router.delete("/api/strategies/{strategy_name}/delete")
def delete_strategy(strategy_name: str) -> dict:
    return StrategyService.delete_strategy(strategy_name)


@router.get("/api/strategies/holdings")
def get_strategy_holdings() -> dict:
    return StrategyService.get_strategy_holdings()


@router.get("/api/orders-trades")
def get_orders_and_trades() -> dict:
    return MainService.get_orders_and_trades()


@router.get("/api/data/portfolios")
def get_portfolio_names() -> dict:
    return MainService.get_portfolio_names()


@router.get("/logs")
def get_logs(limit: int = 200) -> dict:
    return MainService.get_logs(limit)


@router.get("/api/logs")
def get_logs_api() -> dict:
    return MainService.get_logs_api()


@router.post("/api/logs/clear")
def clear_logs() -> dict:
    """Clear log buffer (e.g. Clear button)."""
    return MainService.clear_logs()

