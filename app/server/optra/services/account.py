"""Account service: the account at IB beside the strategies' holdings, the positions check,
settling expired options and booking a missed fill by hand; the risk limits and the halt."""

from optra.infra.remote_client import EngineClient
from optra.services.main import MainService


class AccountService:
    @staticmethod
    def _client() -> EngineClient:
        return MainService._get_client_or_400()

    @staticmethod
    def get_account() -> dict:
        return AccountService._client().get_account()

    @staticmethod
    def reconcile_positions() -> dict:
        return AccountService._client().reconcile_positions()

    @staticmethod
    def settle_expired(portfolio: str, expiry: str, price: float) -> dict:
        return AccountService._client().settle_expired(portfolio, expiry, price)

    @staticmethod
    def book_manually(strategy: str, symbol: str, direction: str, volume: float, price: float,
                      reason: str) -> dict:
        return AccountService._client().book_manually(strategy, symbol, direction, volume, price, reason)

    @staticmethod
    def get_risk() -> dict:
        return AccountService._client().get_risk()

    @staticmethod
    def set_risk_limits(values: dict) -> dict:
        return AccountService._client().set_risk_limits(values)

    @staticmethod
    def halt(reason: str) -> dict:
        return AccountService._client().halt(reason)

    @staticmethod
    def clear_halt() -> dict:
        return AccountService._client().clear_halt()
