"""Unified engine API router: gateway, account, strategies, holdings, logs."""

from datetime import date
from fastapi import APIRouter, Body, HTTPException, Query

from optra.schemas.engine import AddStrategyRequest, RestoreStrategyRequest
from optra.services.account import AccountService
from optra.services.main import MainService
from optra.services.strategy import StrategyService


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


@router.get("/api/account")
def get_account() -> dict:
    """The account at IB beside the strategies' holdings, the positions check, unsettled options."""
    return AccountService.get_account()


@router.post("/api/account/reconcile")
def reconcile_positions() -> dict:
    """Check the strategies' holdings against IB's positions again (after setting them right)."""
    return AccountService.reconcile_positions()


@router.post("/api/account/settle")
def settle_expired(
    portfolio: str = Body(...), expiry: date = Body(...), price: float = Body(..., gt=0)
) -> dict:
    """Settle the portfolio's options that expired on expiry at the underlying's closing price."""
    return AccountService.settle_expired(portfolio, expiry.isoformat(), price)


@router.post("/api/account/book")
def book_manually(
    strategy: str = Body(...), symbol: str = Body(...), direction: str = Body(..., pattern="^(LONG|SHORT)$"),
    volume: float = Body(..., gt=0), price: float = Body(..., ge=0), reason: str = Body(..., min_length=1),
) -> dict:
    """Book a fill the engine missed (the positions check disagrees and IB is right)."""
    return AccountService.book_manually(strategy, symbol, direction, volume, price, reason)


@router.get("/api/risk")
def get_risk() -> dict:
    """The limits every order is checked against, the halt, the orders refused."""
    return AccountService.get_risk()


@router.post("/api/risk/limits")
def set_risk_limits(values: dict[str, float] = Body(..., embed=True)) -> dict:
    """Set risk limits by name; saved by the engine."""
    return AccountService.set_risk_limits(values)


@router.post("/api/risk/halt")
def halt(reason: str = Body("", embed=True)) -> dict:
    """Halt trading: every strategy stopped, working orders cancelled, only closing orders go."""
    return AccountService.halt(reason)


@router.post("/api/risk/clear")
def clear_halt() -> dict:
    """Clear the halt: strategies may be started again."""
    return AccountService.clear_halt()


@router.post("/api/contracts/sync")
def sync_contracts(
    portfolio: str = Body(...), max_dte: int = Body(7, ge=0, le=60)
) -> dict:
    """The portfolio's contracts from IB, for the engine's next start."""
    return MainService.sync_contracts(portfolio, max_dte)


@router.get("/api/strategies")
def list_strategies() -> dict:
    return StrategyService.list_strategies()


@router.get("/api/strategies/meta/strategy-classes")
def get_strategy_classes() -> dict:
    return StrategyService.get_strategy_classes()


@router.get("/api/strategies/meta/settings")
def get_strategy_class_settings(strategy_class: str = Query(..., alias="class")) -> dict:
    """Default settings for strategy class (as the class declares them)."""
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

