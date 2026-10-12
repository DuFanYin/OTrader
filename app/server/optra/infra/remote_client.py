from __future__ import annotations

import json
import os
from typing import Any, Dict, List

import grpc
from google.protobuf.json_format import MessageToDict

from optra.proto import optra_engine_pb2 as pb2
from optra.proto import optra_engine_pb2_grpc as pb2_grpc


def _risk(state) -> Dict[str, Any]:
    """A RiskState as the API returns it."""
    return {
        "status": "ok",
        "halted": state.halted,
        "halt_reason": state.halt_reason,
        "halted_at": state.halted_at,
        "limits": [
            {"name": l.name, "value": l.value, "default": l.default_value, "min": l.min, "max": l.max,
             "integer": l.integer}
            for l in state.limits
        ],
        "refused": state.refused,
        "recent_refusals": list(state.recent_refusals),
    }


class EngineClient:
    """gRPC client for C++ live EngineService (gateway, strategies, orders/trades, holdings)."""

    def __init__(self, target: str | None = None) -> None:
        if target is None:
            target = os.getenv("LIVE_GRPC_TARGET", "localhost:50051")
        self._target = target
        # The saved contracts of a long option chain can pass gRPC's default 4 MB.
        self._channel = grpc.insecure_channel(
            self._target, options=[("grpc.max_receive_message_length", 64 * 1024 * 1024)]
        )
        self._stub = pb2_grpc.EngineServiceStub(self._channel)

    def get_status(self) -> Dict[str, Any]:
        resp = self._stub.GetStatus(pb2.Empty())
        return {
            "status": "running" if resp.running else "stopped",
            "connected": bool(resp.connected),
            "detail": resp.detail,
            "halted": bool(resp.halted),
        }

    def get_orders_and_trades(self, strategy: str | None, limit: int) -> Dict[str, Any]:
        """The orders and trades the engine saved, newest first: the latest `limit` of each (of one
        strategy, or of all), and every strategy name orders or trades are saved under."""
        resp = self._stub.GetOrdersAndTrades(
            pb2.OrdersAndTradesRequest(strategy_name=strategy or "", limit=limit)
        )
        records: List[Dict[str, Any]] = [
            {
                "record_type": "Order",
                "timestamp": o.timestamp,
                "strategy_name": o.strategy_name,
                "id": o.orderid,
                "symbol": o.symbol,
                "direction": o.direction,
                "price": o.price,
                "volume": o.volume,
                "traded": o.traded,
                "status": o.status,
            }
            for o in resp.orders
        ]
        records += [
            {
                "record_type": "Trade",
                "timestamp": t.timestamp,
                "datetime": t.datetime,  # when it filled, as the venue said ("" if unknown)
                "strategy_name": t.strategy_name,
                "id": t.tradeid,
                "symbol": t.symbol,
                "direction": t.direction,
                "price": t.price,
                "volume": t.volume,
                "traded": None,
                "status": None,
            }
            for t in resp.trades
        ]
        records.sort(key=lambda r: r["timestamp"], reverse=True)
        return {"strategies": list(resp.strategies), "records": records}

    def list_saved_contracts(self) -> List[Dict[str, Any]]:
        """The contracts the engine saved (what it trades from its next start)."""
        resp = self._stub.ListSavedContracts(pb2.Empty())
        return [
            {
                "symbol": c.symbol,
                "product": c.product,
                "size": c.size,
                "pricetick": c.pricetick,
                "option_type": c.option_type,
                "option_strike": c.option_strike,
                "option_expiry": c.option_expiry,
                "option_underlying": c.option_underlying,
            }
            for c in resp.contracts
        ]

    def wipe_database(self, what: str) -> Dict[str, int]:
        """Empty "trading" (orders, trades, holdings) or "contracts"; the rows each table lost.
        The engine refuses the trading record while a strategy holds positions."""
        resp = self._stub.WipeDatabase(pb2.WipeDatabaseRequest(what=what))
        return dict(resp.emptied)

    def get_portfolio_names(self) -> List[str]:
        resp = self._stub.ListPortfolios(pb2.Empty())
        return list(resp.portfolios)

    def list_strategies(self) -> List[Dict[str, Any]]:
        resp_iter = self._stub.ListStrategies(pb2.Empty())
        return [
            {
                "strategy_name": s.strategy_name,
                "class_name": s.class_name,
                "portfolio": s.portfolio,
                "status": s.status,
                "settings": dict(s.settings),
            }
            for s in resp_iter
        ]

    def get_strategy_classes(self) -> List[str]:
        resp = self._stub.ListStrategyClasses(pb2.Empty())
        return list(resp.classes)

    def get_strategy_class_defaults(self, strategy_class: str) -> Dict[str, float]:
        """Return default strategy settings for a class (as the class declares them)."""
        req = pb2.GetStrategyClassDefaultsRequest(strategy_class=strategy_class)
        resp = self._stub.GetStrategyClassDefaults(req)
        return dict(resp.settings)

    def get_removed_strategies(self) -> List[str]:
        resp = self._stub.GetRemovedStrategies(pb2.Empty())
        return list(resp.removed_strategies)

    def add_strategy(
        self,
        strategy_class: str,
        portfolio_name: str,
        setting: Dict[str, Any] | None = None,
        strategy_name: str | None = None,
    ) -> Dict[str, Any]:
        req = pb2.AddStrategyRequest(
            strategy_class=strategy_class,
            portfolio_name=portfolio_name,
            setting_json=json.dumps(setting or {}),
            strategy_name=strategy_name or "",
        )
        resp = self._stub.AddStrategy(req)
        return {"status": "ok", "strategy_name": resp.strategy_name}

    def init_strategy(self, strategy_name: str) -> Dict[str, Any]:
        self._stub.InitStrategy(pb2.StrategyNameRequest(strategy_name=strategy_name))
        return {"status": "ok"}

    def start_strategy(self, strategy_name: str) -> Dict[str, Any]:
        self._stub.StartStrategy(pb2.StrategyNameRequest(strategy_name=strategy_name))
        return {"status": "ok"}

    def stop_strategy(self, strategy_name: str) -> Dict[str, Any]:
        self._stub.StopStrategy(pb2.StrategyNameRequest(strategy_name=strategy_name))
        return {"status": "ok"}

    def restore_strategy(self, strategy_name: str) -> Dict[str, Any]:
        self._stub.RestoreStrategy(pb2.StrategyNameRequest(strategy_name=strategy_name))
        return {"status": "ok"}

    def remove_strategy(self, strategy_name: str) -> Dict[str, Any]:
        resp = self._stub.RemoveStrategy(pb2.StrategyNameRequest(strategy_name=strategy_name))
        return {"status": "ok", "removed": bool(resp.removed)}

    def delete_strategy(self, strategy_name: str) -> Dict[str, Any]:
        resp = self._stub.DeleteStrategy(pb2.StrategyNameRequest(strategy_name=strategy_name))
        return {"status": "ok", "deleted": bool(resp.deleted)}

    def get_strategy_holdings(self) -> Dict[str, Any]:
        resp = self._stub.GetStrategyHoldings(pb2.Empty())
        holdings: Dict[str, Any] = {}
        for name, msg in resp.holdings.items():
            holdings[name] = MessageToDict(
                msg,
                preserving_proto_field_name=True,
                always_print_fields_with_no_presence=True,
            )
        return {"holdings": holdings}

    def connect_gateway(self) -> Dict[str, Any]:
        """Connect IBKR gateway (account/order channel); market data is separate."""
        self._stub.ConnectGateway(pb2.Empty())

        status = self.get_status()
        # Confirm C++ side reports IB connected
        if not status.get("connected"):
            detail = status.get("detail") or ""
            return {
                "status": "error",
                "message": "IBKR gateway not connected. Check TWS/Gateway API settings.",
                "gateway": status,
                "detail": detail,
            }

        return {
            "status": "ok",
            "message": "IBKR gateway connected (no market data auto-start).",
            "gateway": status,
        }

    def disconnect_gateway(self) -> Dict[str, Any]:
        try:
            self._stub.StopMarketData(pb2.Empty())
        except grpc.RpcError:
            pass  # disconnecting anyway
        self._stub.DisconnectGateway(pb2.Empty())
        return {
            "status": "ok",
            "message": "Disconnected successfully (gRPC live engine)",
            "gateway": self.get_status(),
        }

    def start_market_data(self) -> Dict[str, Any]:
        """Start market data (gateway state unchanged)."""
        self._stub.StartMarketData(pb2.Empty())
        return {"status": "ok", "message": "Market data started.", "gateway": self.get_status()}

    def stop_market_data(self) -> Dict[str, Any]:
        """Stop market data (gateway state unchanged)."""
        self._stub.StopMarketData(pb2.Empty())
        return {"status": "ok", "message": "Market data stopped.", "gateway": self.get_status()}

    def sync_contracts(self, portfolio: str, max_dte: int) -> Dict[str, Any]:
        """Contracts from IB (the underlying and options within max_dte days), saved by the engine
        for its next start. IB answers within a minute or the engine gives up."""
        # The engine gives up on a sync IB leaves silent for 5 minutes (its first query after a
        # login can be that slow); a long chain, answered all along, takes as long as it takes.
        resp = self._stub.SyncContracts(
            pb2.SyncContractsRequest(portfolio=portfolio, max_dte=max_dte), timeout=1800
        )
        return {"status": "ok", "options": resp.options, "expiries": list(resp.expiries)}

    def get_account(self) -> Dict[str, Any]:
        """The account at IB (its values; its positions, with the strategies' holdings beside them),
        where the positions check stands, and expired options waiting to be settled. When IB cannot
        be asked, from_ib is false, ib_error says why, and only the engine's side is filled."""
        resp = self._stub.GetAccount(pb2.Empty(), timeout=90)
        return {
            "status": "ok",
            "from_ib": resp.from_ib,
            "ib_error": resp.ib_error,
            "account": resp.account,
            "updated": resp.updated,
            "values": [{"key": v.key, "value": v.value, "currency": v.currency} for v in resp.values],
            "positions": [
                MessageToDict(p, preserving_proto_field_name=True, always_print_fields_with_no_presence=True)
                for p in resp.positions
            ],
            "positions_check": resp.positions_check,
            "positions_problem": resp.positions_problem,
            "unsettled": {u.strategy_name: list(u.options) for u in resp.unsettled},
        }

    def reconcile_positions(self) -> Dict[str, Any]:
        """Ask IB for the positions again and check the strategies' holdings against them."""
        resp = self._stub.ReconcilePositions(pb2.Empty(), timeout=90)

        def diffs(items: Any) -> List[Dict[str, Any]]:
            return [{"symbol": d.symbol, "engine": d.engine, "broker": d.broker} for d in items]

        return {"status": "ok", "agree": resp.agree, "mismatches": diffs(resp.mismatches), "external": diffs(resp.external)}

    def settle_expired(self, portfolio: str, expiry: str, price: float) -> Dict[str, Any]:
        """Settle the portfolio's options that expired on expiry (YYYY-MM-DD), the underlying at
        price at that close: the ones the engine could not settle itself."""
        resp = self._stub.SettleExpired(pb2.SettleExpiredRequest(portfolio=portfolio, expiry=expiry, price=price))
        return {"status": "ok", "settlements": resp.settlements}

    def book_manually(self, strategy: str, symbol: str, direction: str, volume: float, price: float,
                      reason: str) -> Dict[str, Any]:
        """Book a fill the engine missed, as an order of the strategy's filled at once."""
        resp = self._stub.BookManually(pb2.BookManuallyRequest(
            strategy=strategy, symbol=symbol, direction=direction, volume=volume, price=price, reason=reason))
        return {"status": "ok", "orderid": resp.orderid}

    def get_risk(self) -> Dict[str, Any]:
        """The limits every order is checked against, the halt, the orders refused."""
        return _risk(self._stub.GetRisk(pb2.Empty()))

    def set_risk_limits(self, values: Dict[str, float]) -> Dict[str, Any]:
        return _risk(self._stub.SetRiskLimits(pb2.SetRiskLimitsRequest(values=values)))

    def halt(self, reason: str) -> Dict[str, Any]:
        """Halt trading: every strategy stopped, working orders cancelled, only closing orders go."""
        return _risk(self._stub.Halt(pb2.HaltRequest(reason=reason)))

    def clear_halt(self) -> Dict[str, Any]:
        return _risk(self._stub.ClearHalt(pb2.Empty()))

    def stream_logs(self):
        """Yield log lines from C++ via gRPC StreamLogs (server-formatted)."""
        try:
            for msg in self._stub.StreamLogs(pb2.Empty()):
                # msg.line is a plain text line
                yield msg.line
        except grpc.RpcError:
            # Connection dropped or stream ended
            return

