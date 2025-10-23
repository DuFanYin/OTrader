import sqlite3
from collections.abc import Callable
from datetime import UTC, datetime
from pathlib import Path
from threading import Lock
from typing import TYPE_CHECKING, Concatenate, ParamSpec, TypeVar, cast

from engines.engine_log import DEBUG, ERROR, INFO, WARNING
from utilities.base_engine import BaseEngine
from utilities.constant import LOCAL_TZ, Exchange, OptionType, Product
from utilities.event import EVENT_CONTRACT, EVENT_LOG
from utilities.object import ContractData, LogData, OrderData, TradeData
from utilities.sql_schema import CREATE_CONTRACT_EQUITY_TABLE, CREATE_CONTRACT_OPTION_TABLE, CREATE_ORDERS_TABLE, CREATE_TRADES_TABLE
from utilities.utility import get_file_path

from .engine_event import Event

if TYPE_CHECKING:
    from engines.engine_event import EventEngine
    from engines.engine_main import MainEngine

    from .engine_event import Event

APP_NAME = "Database"
DB_TZ = LOCAL_TZ

REQUIRED_SYMBOLS = ["SPY", "AAPL", "GOOGL"]

AUTOMATIC_UPDATE_SYMBOLS = False


P = ParamSpec("P")
R = TypeVar("R")


def with_db_lock(func: Callable[Concatenate["DatabaseEngine", P], R]) -> Callable[Concatenate["DatabaseEngine", P], R]:
    def wrapper(self: "DatabaseEngine", *args: P.args, **kwargs: P.kwargs) -> R:
        with self.lock:
            return func(self, *args, **kwargs)

    return cast(Callable[Concatenate["DatabaseEngine", P], R], wrapper)


class DatabaseEngine(BaseEngine):
    def __init__(self, main_engine: "MainEngine", event_engine: "EventEngine") -> None:
        super().__init__(main_engine, event_engine, APP_NAME)

        self.db_path = Path(get_file_path("trading.db"))
        db_exists = self.db_path.exists()

        self.conn = sqlite3.connect(str(self.db_path), check_same_thread=False)
        self.cursor = self.conn.cursor()
        self.lock = Lock()

        if not db_exists:
            self.write_log("Database file not found, creating new database", INFO)
            self._create_tables()
        else:
            self._cleanup_expired_options()

        if AUTOMATIC_UPDATE_SYMBOLS:
            self._check_required_symbols()

    # ====================
    # Database Setup & Maintenance
    # ====================

    @with_db_lock
    def _create_tables(self) -> None:
        tables = {
            "contract_equity": CREATE_CONTRACT_EQUITY_TABLE,
            "contract_option": CREATE_CONTRACT_OPTION_TABLE,
            "orders": CREATE_ORDERS_TABLE,
            "trades": CREATE_TRADES_TABLE,
        }

        for table_name, create_sql in tables.items():
            self.write_log(f"Creating table: {table_name}", DEBUG)
            self.cursor.execute(create_sql)

        self.conn.commit()
        self.write_log("All tables created successfully", INFO)

    @with_db_lock
    def _cleanup_expired_options(self) -> None:
        """Remove expired option contracts from the database."""
        try:
            today_utc = datetime.now(UTC).date()

            self.cursor.execute("SELECT COUNT(*) FROM contract_option WHERE expiry < ?", (today_utc.strftime("%Y-%m-%d"),))
            expired_count = self.cursor.fetchone()[0]

            if expired_count > 0:
                self.cursor.execute("DELETE FROM contract_option WHERE expiry < ?", (today_utc.strftime("%Y-%m-%d"),))

                self.conn.commit()
                self.write_log(f"Cleaned up {expired_count} expired option contracts", INFO)

        except Exception as e:
            self.write_log(f"Failed to cleanup expired options: {e}", ERROR)
            self.conn.rollback()

    @with_db_lock
    def _check_required_symbols(self) -> None:
        """Check if required symbols have equity data, query portfolio if missing."""
        try:
            missing_equity = []

            for symbol in REQUIRED_SYMBOLS:
                self.cursor.execute("SELECT COUNT(*) FROM contract_equity WHERE symbol LIKE ?", (f"{symbol}-%",))
                equity_count = self.cursor.fetchone()[0]

                if equity_count == 0:
                    missing_equity.append(symbol)

            if missing_equity:
                self.write_log(f"WARNING: Missing equity contracts for symbols: {', '.join(missing_equity)}", WARNING)

                for symbol in missing_equity:
                    self.write_log(f"Querying portfolio for missing symbol: {symbol}", INFO)
                    self.query_portfolio(symbol)
            else:
                self.write_log("All required symbols have equity data", INFO)

        except Exception as e:
            self.write_log(f"Failed to check required symbols: {e}", ERROR)

    # ====================
    # Contract Data Management
    # ====================

    @with_db_lock
    def save_contract_data(self, contract: "ContractData", symbol_key: str) -> None:
        try:
            base_data_values = [
                symbol_key,
                contract.symbol,
                contract.exchange.value,
                contract.product.value,
                float(contract.size),
                float(contract.pricetick),
                float(contract.min_volume),
                1 if contract.net_position else 0,
                1 if contract.history_data else 0,
                1 if contract.stop_supported else 0,
                contract.gateway_name,
                contract.con_id,
                contract.trading_class,
                contract.name,
                (float(contract.max_volume) if contract.max_volume is not None else None),
                None,
            ]

            if contract.product == Product.OPTION:
                option_data_values = base_data_values + [
                    contract.option_portfolio,
                    (contract.option_type.value if contract.option_type is not None else None),
                    (float(contract.option_strike) if contract.option_strike is not None else 0.0),
                    contract.option_index,
                    (contract.option_expiry.strftime("%Y-%m-%d") if contract.option_expiry is not None else None),
                    contract.option_underlying,
                ]

                self.cursor.execute(
                    """
                    INSERT OR REPLACE INTO contract_option (
                        symbol, symbol, exchange, product,
                        size, pricetick, min_volume, net_position,
                        history_data, stop_supported, gateway_name, con_id,
                        trading_class, name, max_volume, extra,
                        portfolio, type, strike,
                        strike_index, expiry, underlying
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                    tuple(option_data_values),
                )
            else:
                self.cursor.execute(
                    """
                    INSERT OR REPLACE INTO contract_equity (
                        symbol, symbol, exchange, product,
                        size, pricetick, min_volume, net_position,
                        history_data, stop_supported, gateway_name, con_id,
                        trading_class, name, max_volume, extra
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                    tuple(base_data_values),
                )

            self.conn.commit()

        except Exception as e:
            self.write_log(f"Failed to save ContractData to database: {e}", ERROR)

    @with_db_lock
    def load_contract_data(self, symbol_key: str | None = None) -> dict[str | None, ContractData]:
        try:
            contracts = {}

            equity_query = """
                SELECT
                    symbol, symbol, exchange, product,
                    size, pricetick, min_volume, net_position,
                    history_data, stop_supported, gateway_name, con_id,
                    trading_class, name, max_volume, extra
                FROM contract_equity
            """

            if symbol_key:
                equity_query += " WHERE symbol = ?"
                self.cursor.execute(equity_query, (symbol_key,))
            else:
                self.cursor.execute(equity_query)

            equity_rows = self.cursor.fetchall()

            for row in equity_rows:
                (
                    row_symbol,
                    symbol,
                    exchange,
                    product,
                    size,
                    pricetick,
                    min_volume,
                    net_position,
                    history_data,
                    stop_supported,
                    gateway_name,
                    con_id,
                    trading_class,
                    name,
                    max_volume,
                    extra,
                ) = row

                contract = ContractData(
                    symbol=symbol,
                    exchange=Exchange(exchange),
                    product=Product(product),
                    size=float(size),
                    pricetick=float(pricetick),
                    min_volume=float(min_volume),
                    net_position=bool(net_position),
                    history_data=bool(history_data),
                    stop_supported=bool(stop_supported),
                    gateway_name=gateway_name,
                    con_id=con_id,
                    trading_class=trading_class,
                    name=name,
                    max_volume=(float(max_volume) if max_volume is not None else None),
                )

                contracts[row_symbol] = contract

            option_query = """
                SELECT
                    symbol, symbol, exchange, product,
                    size, pricetick, min_volume, net_position,
                    history_data, stop_supported, gateway_name, con_id,
                    trading_class, name, max_volume, extra,
                    portfolio, type, strike,
                    strike_index, expiry, underlying
                FROM contract_option
            """

            if symbol_key:
                option_query += " WHERE symbol = ?"
                self.cursor.execute(option_query, (symbol_key,))
            else:
                self.cursor.execute(option_query)

            option_rows = self.cursor.fetchall()

            for row in option_rows:
                (
                    row_symbol,
                    symbol,
                    exchange,
                    product,
                    size,
                    pricetick,
                    min_volume,
                    net_position,
                    history_data,
                    stop_supported,
                    gateway_name,
                    con_id,
                    trading_class,
                    name,
                    max_volume,
                    extra,
                    portfolio,
                    option_type,
                    strike,
                    strike_index,
                    expiry,
                    underlying,
                ) = row

                contract = ContractData(
                    symbol=symbol,
                    exchange=Exchange(exchange),
                    product=Product(product),
                    size=float(size),
                    pricetick=float(pricetick),
                    min_volume=float(min_volume),
                    net_position=bool(net_position),
                    history_data=bool(history_data),
                    stop_supported=bool(stop_supported),
                    gateway_name=gateway_name,
                    con_id=con_id,
                    trading_class=trading_class,
                    name=name,
                    max_volume=(float(max_volume) if max_volume is not None else None),
                )

                contract.option_portfolio = portfolio
                if option_type:
                    contract.option_type = OptionType(option_type)
                contract.option_strike = float(strike) if strike else None
                contract.option_index = strike_index
                if expiry:
                    contract.option_expiry = datetime.strptime(expiry, "%Y-%m-%d")
                contract.option_underlying = underlying

                contracts[row_symbol] = contract

            return contracts

        except Exception as e:
            self.write_log(f"Failed to load ContractData from database: {e}", ERROR)
            return {}

    def load_contracts(self, event_engine: "EventEngine") -> None:
        """Load contracts from database and emit events to update main engine cache"""
        try:
            contracts = self.load_contract_data()
            product_counts: dict[str, int] = {}

            for contract in contracts.values():
                event = Event(EVENT_CONTRACT, contract)
                event_engine.put(event)

                product_type = contract.product.value
                product_counts[product_type] = product_counts.get(product_type, 0) + 1

            total_contracts = sum(product_counts.values())
            if total_contracts > 0:
                equity_count = product_counts.get("EQUITY", 0)
                index_count = product_counts.get("INDEX", 0)
                option_count = product_counts.get("OPTION", 0)

                portfolio_count = equity_count + index_count
                self.write_log(f"Loaded {portfolio_count} portfolio with {option_count} options", INFO)
            else:
                self.write_log("No contracts loaded", INFO)

        except Exception as e:
            self.write_log(f"Failed to load contracts from database: {e}", ERROR)

    # ====================
    # Trading Data Management
    # ====================

    @with_db_lock
    def save_order_data(self, strategy_name: str, order: OrderData) -> None:
        try:
            legs_info = ""
            if order.is_combo and order.legs:
                legs_info = "|".join(
                    [f"con_id:{leg.con_id},ratio:{leg.ratio},dir:{leg.direction.value},symbol:{leg.symbol or 'N/A'}" for leg in order.legs]
                )

            data = (
                datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                strategy_name,
                order.orderid,
                order.symbol,
                order.exchange.value,
                order.trading_class,
                order.type.value,
                order.direction.value if order.direction else "N/A",
                float(order.price) if order.price is not None else 0.0,
                float(order.volume),
                float(order.traded),
                order.status.value,
                (order.datetime.strftime("%Y-%m-%d %H:%M:%S") if order.datetime else "N/A"),
                order.reference,
                1 if order.is_combo else 0,
                legs_info,
            )

            self.cursor.execute(
                """
                INSERT OR REPLACE INTO orders (
                    timestamp, strategy_name, orderid, symbol, exchange,
                    trading_class, type, direction, price, volume,
                    traded, status, datetime, reference, is_combo, legs_info
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
                data,
            )

            self.conn.commit()

        except Exception as e:
            self.write_log(f"Failed to save order to database: {e}", ERROR)

    @with_db_lock
    def save_trade_data(self, strategy_name: str, trade: TradeData) -> None:
        try:
            data = (
                datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                strategy_name,
                trade.tradeid,
                trade.symbol,
                trade.exchange.value,
                trade.orderid,
                trade.direction.value if trade.direction else "N/A",
                float(trade.price),
                float(trade.volume),
                (trade.datetime.strftime("%Y-%m-%d %H:%M:%S") if trade.datetime else "N/A"),
            )

            self.cursor.execute(
                """
                INSERT OR REPLACE INTO trades (
                    timestamp, strategy_name, tradeid, symbol, exchange,
                    orderid, direction, price, volume, datetime
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
                data,
            )

            self.conn.commit()

        except Exception as e:
            self.write_log(f"Failed to save trade to database: {e}", ERROR)

    @with_db_lock
    def get_all_history_orders(self) -> list[tuple]:
        """Return raw tuples of all rows from orders table (no formatting)."""
        try:
            self.cursor.execute(
                """
                SELECT * FROM orders
                ORDER BY timestamp ASC
                """
            )
            return self.cursor.fetchall()
        except Exception as e:
            self.write_log(f"Failed to fetch orders: {e}", ERROR)
            return []

    @with_db_lock
    def get_all_history_trades(self) -> list[tuple]:
        """Return raw tuples of all rows from trades table (no formatting)."""
        try:
            self.cursor.execute(
                """
                SELECT * FROM trades
                ORDER BY timestamp ASC
                """
            )
            return self.cursor.fetchall()
        except Exception as e:
            self.write_log(f"Failed to fetch trades: {e}", ERROR)
            return []

    @with_db_lock
    def wipe_trading_data(self) -> None:
        """Wipe all order and trade data while preserving contract data"""
        try:
            self.cursor.execute("DELETE FROM orders")
            self.cursor.execute("DELETE FROM trades")

            self.cursor.execute("SELECT COUNT(*) FROM orders")
            orders_count = self.cursor.fetchone()[0]
            self.cursor.execute("SELECT COUNT(*) FROM trades")
            trades_count = self.cursor.fetchone()[0]

            self.conn.commit()

            self.write_log(f"Trading data wiped successfully. Remaining records: {orders_count} orders, {trades_count} trades", INFO)

        except Exception as e:
            self.write_log(f"Failed to wipe trading data: {e}", ERROR)
            self.conn.rollback()

    # ====================
    # Portfolio Management
    # ====================

    def query_portfolio(self, symbol: str) -> None:
        """Query contract details and option portfolio for a given symbol."""
        try:
            self.write_log(f"Querying portfolio for {symbol}", INFO)
            self.main_engine.ib_gateway.query_portfolio(symbol)

        except Exception as e:
            self.write_log(f"Failed to query portfolio for {symbol}: {e}", ERROR)

    @with_db_lock
    def delete_portfolio(self, symbol: str) -> None:
        """Delete equity and all related options for a specific symbol."""
        try:
            self.cursor.execute("DELETE FROM contract_equity WHERE symbol LIKE ?", (f"{symbol}-%",))
            equity_deleted = self.cursor.rowcount

            self.cursor.execute("DELETE FROM contract_option WHERE symbol LIKE ?", (f"{symbol}-%",))
            options_deleted = self.cursor.rowcount

            self.conn.commit()

            self.write_log(f"Deleted portfolio for {symbol}: {equity_deleted} equity contracts, {options_deleted} option contracts", INFO)

        except Exception as e:
            self.write_log(f"Failed to delete portfolio for {symbol}: {e}", ERROR)
            self.conn.rollback()

    # ====================
    # Utility Methods
    # ====================

    def write_log(self, msg: str, level: int = INFO) -> None:
        log = LogData(msg=msg, level=level, gateway_name=APP_NAME)
        event = Event(EVENT_LOG, log)
        self.event_engine.put(event)

    def close(self) -> None:
        self.conn.close()
