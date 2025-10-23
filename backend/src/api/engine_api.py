"""Unified engine routes exposed through MainEngine wrapper.

This consolidates previous routers:
- main_engine_api.py
- strategy_engine_api.py
- portfolio_engine_api.py
- database_engine_api.py
- log_engine_api.py

Static file routes remain in `static_api.py`.
"""

from fastapi import APIRouter

from ..services.models import AddStrategyRequest, RestoreStrategyRequest
from ..services.service_main import MainService
from ..services.service_strategy import StrategyService

router = APIRouter()


# ===================== Gateway Management =====================
@router.post("/api/gateway/connect")
def connect_gateway() -> dict:
    return MainService.connect_gateway()


@router.post("/api/gateway/disconnect")
def disconnect_gateway() -> dict:
    return MainService.disconnect_gateway()


@router.get("/api/gateway/status")
def get_gateway_status() -> dict:
    return MainService.get_gateway_status()


# ===================== Strategies =====================
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


# ===================== Portfolio =====================
# Portfolio endpoints removed - only portfolio names needed


@router.get("/api/strategies/holdings")
def get_strategy_holdings() -> dict:
    return StrategyService.get_strategy_holdings()


# ===================== Data Management =====================
@router.get("/api/orders-trades")
def get_orders_and_trades() -> dict:
    return MainService.get_orders_and_trades()


@router.get("/api/data/portfolios")
def get_portfolio_names() -> dict:
    return MainService.get_portfolio_names()


# ===================== Logs =====================
@router.get("/logs")
def get_logs(limit: int = 200) -> dict:
    return MainService.get_logs(limit)


@router.get("/api/logs")
def get_logs_api() -> dict:
    return MainService.get_logs_api()
