from engines.engine_log import INFO
from utilities.event import EVENT_ACCOUNT, EVENT_LOG, EVENT_POSITION
from utilities.object import AccountData, CancelRequest, ContractData, LogData, OrderData, OrderRequest, PositionData, TradeData
from utilities.portfolio import PortfolioData

from .engine_data import MarketDataEngine
from .engine_db import DatabaseEngine
from .engine_event import Event, EventEngine
from .engine_gateway import IbGateway
from .engine_log import LogEngine
from .engine_strategy import OptionStrategyEngine

APP_NAME = "Main"


class MainEngine:
    """Main Engine"""

    def __init__(self, event_engine: EventEngine | None = None) -> None:
        self.event_engine: EventEngine = event_engine or EventEngine()
        self.event_engine.start()

        self.log_engine: LogEngine = LogEngine(self, self.event_engine)
        self.db_engine: DatabaseEngine = DatabaseEngine(self, self.event_engine)

        self.positions: dict[str, PositionData] = {}
        self.accounts: dict[str, AccountData] = {}

        self.register_event()

        self.ib_gateway: IbGateway = IbGateway(self, self.event_engine)
        self.market_data_engine: MarketDataEngine = MarketDataEngine(self, self.event_engine)
        self.option_strategy_engine: OptionStrategyEngine = OptionStrategyEngine(self, self.event_engine)

        self.db_engine.load_contracts(self.event_engine)

        self.write_log("Main engine initialization successful", INFO)

    # ---------------- Market data wrapper interface (provides unified entry point) ----------------

    def start_market_data_update(self) -> None:
        self.market_data_engine.start_market_data_update()

    def stop_market_data_update(self) -> None:
        self.market_data_engine.stop_market_data_update()

    def subscribe_chains(self, strategy_name: str, chain_symbols: list[str]) -> None:
        self.market_data_engine.subscribe_chains(strategy_name, chain_symbols)

    def unsubscribe_chains(self, strategy_name: str) -> None:
        self.market_data_engine.unsubscribe_chains(strategy_name)

    def get_portfolio(self, portfolio_name: str) -> PortfolioData | None:
        return self.market_data_engine.get_portfolio(portfolio_name)

    def get_all_portfolio_names(self) -> list[str]:
        return self.market_data_engine.get_all_portfolio_names()

    def get_contract(self, symbol: str) -> ContractData | None:
        return self.market_data_engine.get_contract(symbol)

    def get_all_contracts(self) -> list[ContractData]:
        return self.market_data_engine.get_all_contracts()

    # ---------------- database engine wrapper interface (provides unified entry point) -------------

    def save_trade_data(self, strategy_name: str, trade: TradeData) -> None:
        self.db_engine.save_trade_data(strategy_name, trade)

    def save_order_data(self, strategy_name: str, order: OrderData) -> None:
        self.db_engine.save_order_data(strategy_name, order)

    def get_all_history_orders(self) -> list[tuple]:
        return self.db_engine.get_all_history_orders()

    def get_all_history_trades(self) -> list[tuple]:
        return self.db_engine.get_all_history_trades()

    def wipe_trading_data(self) -> None:
        """Wipe all order and trade data while preserving contract data"""
        self.db_engine.wipe_trading_data()

    # ---------------- Gateway wrapper interface (provides unified entry point) ---------------------

    def connect(self) -> None:
        self.ib_gateway.connect()

    def disconnect(self) -> None:
        self.ib_gateway.disconnect()

    def cancel_order(self, req: CancelRequest) -> None:
        self.ib_gateway.cancel_order(req)

    def send_order(self, req: OrderRequest) -> str:
        return self.ib_gateway.send_order(req)

    def query_account(self) -> None:
        self.ib_gateway.query_account()

    def query_position(self) -> None:
        self.ib_gateway.query_position()

    # ---------------- event handlers & query (inline) ---------------------------------------------

    def register_event(self) -> None:
        ee = self.event_engine
        ee.register(EVENT_POSITION, self.process_position_event)
        ee.register(EVENT_ACCOUNT, self.process_account_event)

    def process_position_event(self, event: Event) -> None:
        pos: PositionData = event.data
        self.positions[pos.positionid] = pos

    def process_account_event(self, event: Event) -> None:
        acc: AccountData = event.data
        self.accounts[acc.accountid] = acc

    # ---------------- OMS query interface (provided directly in main engine) -----------------------

    def get_position(self, positionid: str) -> PositionData | None:
        return self.positions.get(positionid)

    def get_account(self, accountid: str) -> AccountData | None:
        return self.accounts.get(accountid)

    def get_all_positions(self) -> list[PositionData]:
        return list(self.positions.values())

    def get_all_accounts(self) -> list[AccountData]:
        return list(self.accounts.values())

    # ---------------- Strategy engine query interface ----------------------------------------------

    def get_order(self, orderid: str) -> OrderData | None:
        return self.option_strategy_engine.get_order(orderid)

    def get_trade(self, tradeid: str) -> TradeData | None:
        return self.option_strategy_engine.get_trade(tradeid)

    def get_all_orders(self) -> list[OrderData]:
        return self.option_strategy_engine.get_all_orders()

    def get_all_trades(self) -> list[TradeData]:
        return self.option_strategy_engine.get_all_trades()

    def get_all_active_orders(self) -> list[OrderData]:
        return self.option_strategy_engine.get_all_active_orders()

    # ---------------- Other functions --------------------------------------------------------------

    def write_log(self, msg: str, level: int = INFO) -> None:
        """Write log to event system"""
        log = LogData(msg=msg, level=level, gateway_name=APP_NAME)
        event = Event(EVENT_LOG, log)
        self.event_engine.put(event)

    def close(self) -> None:
        """Gracefully close all modules"""
        self.option_strategy_engine.close()
        self.db_engine.close()
        self.ib_gateway.disconnect()
        self.event_engine.stop()
