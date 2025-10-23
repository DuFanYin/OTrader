"""Main engine service (gateway, database, portfolio, logs)."""

from fastapi import HTTPException

from ..infrastructure.state import AppState


class MainService:
    """Main service for gateway, database, portfolio, and log operations."""

    # ===================== Gateway Operations =====================

    @staticmethod
    def connect_gateway() -> dict:
        if AppState.engine is None:
            raise HTTPException(status_code=400, detail="system not started")
        try:
            AppState.engine.connect()

            # Start market data engine after connection
            try:
                AppState.engine.start_market_data_update()
                return {"status": "ok", "message": "Connected successfully with market data feed"}
            except Exception as e:
                return {"status": "ok", "message": f"Connected successfully but market data failed: {str(e)}"}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def disconnect_gateway() -> dict:
        if AppState.engine is None:
            raise HTTPException(status_code=400, detail="system not started")
        try:
            AppState.engine.disconnect()
            return {"status": "ok", "message": "Disconnected successfully"}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def get_gateway_status() -> dict:
        if AppState.engine is None:
            return {"status": "stopped", "connected": False}
        try:
            ib_gateway = AppState.engine.ib_gateway
            connected = hasattr(ib_gateway.api, "status") and ib_gateway.api.status
            return {"status": "running", "connected": connected}
        except Exception:
            return {"status": "error", "connected": False}

    # ===================== Database Operations =====================

    @staticmethod
    def get_orders_and_trades() -> dict:
        """Get combined orders and trades, sorted by timestamp (most recent first)."""
        if AppState.engine is None:
            return {"records": []}
        try:
            raw_orders = AppState.engine.get_all_history_orders()
            raw_trades = AppState.engine.get_all_history_trades()

            # Process orders: convert tuples to formatted objects
            processed_orders = []
            for order_tuple in raw_orders:
                # SQL schema: timestamp, strategy_name, orderid, symbol, exchange, trading_class, type, direction, price, volume, traded, status, datetime, reference, is_combo, legs_info
                processed_order = {
                    "record_type": "Order",
                    "timestamp": order_tuple[0],
                    "strategy_name": order_tuple[1],
                    "orderid": order_tuple[2],
                    "symbol": order_tuple[3],
                    "exchange": order_tuple[4],
                    "trading_class": order_tuple[5],
                    "type": order_tuple[6],
                    "direction": order_tuple[7],
                    "price": float(order_tuple[8]) if order_tuple[8] is not None else None,
                    "volume": float(order_tuple[9]) if order_tuple[9] is not None else None,
                    "traded": float(order_tuple[10]) if order_tuple[10] is not None else None,
                    "status": order_tuple[11],
                    "datetime": order_tuple[12],
                    "reference": order_tuple[13],
                    "is_combo": bool(order_tuple[14]) if order_tuple[14] is not None else False,
                    "legs_info": order_tuple[15],
                    "formatted_price": f"{order_tuple[8]:.2f}" if order_tuple[8] is not None else "-",
                    "formatted_quantity": str(order_tuple[10] or order_tuple[9])
                    if (order_tuple[10] is not None or order_tuple[9] is not None)
                    else "-",
                    "formatted_timestamp": MainService._format_timestamp(order_tuple[0]),
                }
                processed_orders.append(processed_order)

            # Process trades: convert tuples to formatted objects
            processed_trades = []
            for trade_tuple in raw_trades:
                # SQL schema: timestamp, strategy_name, tradeid, symbol, exchange, orderid, direction, price, volume, datetime
                processed_trade = {
                    "record_type": "Trade",
                    "timestamp": trade_tuple[0],
                    "strategy_name": trade_tuple[1],
                    "tradeid": trade_tuple[2],
                    "symbol": trade_tuple[3],
                    "exchange": trade_tuple[4],
                    "orderid": trade_tuple[5],
                    "direction": trade_tuple[6],
                    "price": float(trade_tuple[7]) if trade_tuple[7] is not None else None,
                    "volume": float(trade_tuple[8]) if trade_tuple[8] is not None else None,
                    "datetime": trade_tuple[9],
                    "formatted_price": f"{trade_tuple[7]:.2f}" if trade_tuple[7] is not None else "-",
                    "formatted_quantity": str(trade_tuple[8]) if trade_tuple[8] is not None else "-",
                    "formatted_timestamp": MainService._format_timestamp(trade_tuple[0]),
                }
                processed_trades.append(processed_trade)

            # Combine orders and trades
            all_records = processed_orders + processed_trades

            # Sort by timestamp (most recent first)
            all_records.sort(key=lambda x: x["timestamp"], reverse=True)

            return {"records": all_records}
        except Exception as e:
            return {"error": str(e)}

    # ===================== Portfolio Operations =====================

    @staticmethod
    def get_portfolio_names() -> dict:
        """Get all portfolio names from the main engine."""
        if AppState.engine is None:
            return {"portfolios": []}
        try:
            names = AppState.engine.get_all_portfolio_names()
            return {"portfolios": names}
        except Exception as e:
            return {"error": str(e)}

    # ===================== Log Operations =====================

    @staticmethod
    def get_logs(limit: int = 200) -> dict:
        if limit <= 0:
            limit = 200
        return {"logs": AppState.log_buffer[-limit:]}

    @staticmethod
    def get_logs_api() -> dict:
        return {"logs": AppState.log_buffer[-50:]}

    # ===================== Utility Methods =====================

    @staticmethod
    def _format_timestamp(timestamp_str: str) -> str:
        """Format timestamp for display."""
        if not timestamp_str:
            return "-"
        try:
            # Parse the timestamp string
            from datetime import datetime

            dt = datetime.fromisoformat(timestamp_str.replace("Z", "+00:00"))
            return dt.strftime("%m/%d/%Y %H:%M:%S")
        except Exception:
            return timestamp_str
