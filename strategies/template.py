from abc import ABC, abstractmethod
from typing import TYPE_CHECKING

from engines.engine_event import Event
from engines.engine_log import INFO
from utilities.constant import ComboType, Direction, OrderType
from utilities.event import EVENT_LOG
from utilities.object import ComboPositionData, Leg, LogData, OptionPositionData, OrderData, StrategyHolding, TradeData
from utilities.portfolio import ChainData, OptionData, PortfolioData, UnderlyingData

if TYPE_CHECKING:
    from engines.engine_strategy import OptionStrategyEngine


class OptionStrategyTemplate(ABC):
    """Option strategy template with portfolio management and order interfaces"""

    author: str = "Your Name"
    parameters: list[str] = []
    variables: list[str] = ["inited", "started", "timer_trigger", "error"]
    timer_trigger: int = 10

    def __init__(self, strategy_engine: "OptionStrategyEngine", strategy_name: str, portfolio_name: str, setting: dict):
        self.engine = strategy_engine
        self.strategy_name = strategy_name
        self.portfolio_name = portfolio_name

        portfolio = self.engine.get_portfolio(portfolio_name)
        if portfolio is None:
            raise ValueError(f"Portfolio {portfolio_name} not found")

        self.portfolio: PortfolioData = portfolio
        self.chain_map: dict[str, ChainData] = {}
        self.underlying: UnderlyingData | None = self.portfolio.underlying

        self.holding: StrategyHolding

        self.inited = False
        self.started = False
        self.error = False
        self.error_msg = ""

        self._timer_cnt = 0
        if "timer_trigger" in setting:
            self.timer_trigger = setting["timer_trigger"]

        self.update_setting(setting)
        self.write_log(f"Strategy {strategy_name} created for portfolio {self.portfolio_name}")

    @abstractmethod
    def on_init_logic(self) -> None:
        pass

    @abstractmethod
    def on_stop_logic(self) -> None:
        pass

    @abstractmethod
    def on_timer_logic(self) -> None:
        pass

    def on_init(self) -> None:
        self.inited = True
        self.on_init_logic()
        self.put_event()

    def on_start(self) -> None:
        self.started = True
        self.put_event()

    def on_stop(self) -> None:
        self.started = False
        self.on_stop_logic()
        self.put_event()

    def on_timer(self) -> None:
        if not self.started:
            return

        self._timer_cnt += 1
        if self._timer_cnt >= self.timer_trigger:
            self._timer_cnt = 0
            self.on_timer_logic()

    def update_setting(self, setting: dict) -> None:
        for k in self.parameters:
            if k in setting:
                setattr(self, k, setting[k])

    def get_parameters(self) -> dict:
        return {k: getattr(self, k) for k in self.parameters}

    def get_variables(self) -> dict:
        return {k: getattr(self, k) for k in self.variables}

    def get_strategy_status(self) -> dict:
        """Get current strategy status for UI display"""
        return {
            "strategy_name": self.strategy_name,
            "inited": self.inited,
            "started": self.started,
            "error": self.error,
            "error_msg": self.error_msg,
        }

    def get_holding_data(self) -> StrategyHolding | None:
        """Get current holding data"""
        return self.engine.get_strategy_holding(self.strategy_name)

    def restore_holding_data(self, holding_data: dict | StrategyHolding) -> None:
        """Restore holding data from saved state"""
        if isinstance(holding_data, dict):
            self.engine.position_engine.load_serialized_holding(self.strategy_name, holding_data)
        else:
            raise NotImplementedError("Cannot restore from StrategyHolding object directly")

    def on_order(self, order: OrderData) -> None:
        self.write_log(
            f"Order {order.orderid}: {order.direction.value if order.direction else ''} {order.volume} @ {order.price} [{order.status.value}]"
        )

    def on_trade(self, trade: TradeData) -> None:
        self.write_log(f"Trade {trade.tradeid}: {trade.direction.value if trade.direction else ''} {trade.volume} @ {trade.price}")

    def subscribe_chains(self, chain_symbols: list[str]) -> None:
        if not chain_symbols:
            self.write_log("No chains provided for subscription")
            return

        self.chain_symbols = chain_symbols
        self.engine.subscribe_chains(self.strategy_name, chain_symbols)
        # Build local references to chains for this strategy
        self.chain_map = {}
        for symbol in chain_symbols:
            chain = self.portfolio.chains.get(symbol)
            if chain:
                self.chain_map[symbol] = chain
        self.write_log(f"Subscribed to chains: {chain_symbols}")

    def get_chain(self, chain_symbol: str) -> ChainData | None:
        """Get a locally-referenced ChainData by symbol for this strategy."""
        return self.chain_map.get(chain_symbol)

    # ===================== hedging related helper methods =======================

    def register_hedging(self, timer_trigger: int = 5, delta_target: int = 0, delta_range: int = 0) -> None:
        """Register this strategy for delta hedging"""
        self.engine.hedge_engine.register_strategy(
            strategy=self, timer_trigger=timer_trigger, delta_target=delta_target, delta_range=delta_range
        )

    def unregister_hedging(self) -> None:
        """Unregister this strategy from delta hedging"""
        self.engine.hedge_engine.unregister_strategy(self.strategy_name)

    # ===================== order related helper methods =======================

    def cancel_order(self, orderid: str) -> None:
        self.engine.cancel_order(self, orderid)

    def cancel_all(self) -> None:
        self.engine.cancel_all(self)

    def _submit(
        self,
        legs: list[Leg] | None = None,
        symbol: str = "",
        direction: Direction = Direction.LONG,
        price: float = 0,
        volume: float = 0,
        order_type: OrderType = OrderType.LIMIT,
        combo_type: ComboType | None = None,
        combo_sig: str | None = None,
        reference: str | None = None,
    ) -> list[str]:
        return self.engine.send_order(
            strategy=self,
            symbol=symbol,
            direction=direction,
            price=price,
            volume=volume,
            combo_type=combo_type,
            combo_sig=combo_sig,
            order_type=order_type,
            legs=legs,
            reference=reference,
        )

    def underlying_order(
        self, direction: Direction, price: float, volume: float = 1, order_type: OrderType = OrderType.MARKET, reference: str | None = None
    ) -> list[str]:
        """Underlying order with specified direction."""
        if self.underlying is None:
            raise ValueError("Underlying not set for this strategy")
        return self._submit(
            symbol=self.underlying.symbol, direction=direction, price=price, volume=volume, order_type=order_type, reference=reference
        )

    def option_order(
        self,
        option_data: "OptionData",
        direction: Direction,
        price: float,
        volume: float = 1,
        order_type: OrderType = OrderType.MARKET,
        reference: str | None = None,
    ) -> list[str]:
        """Option order with specified direction."""
        return self._submit(
            symbol=option_data.symbol, direction=direction, price=price, volume=volume, order_type=order_type, reference=reference
        )

    def combo_order(
        self,
        combo_type: ComboType,
        option_data: dict[str, "OptionData"],
        direction: Direction,
        price: float,
        volume: float = 1,
        order_type: OrderType = OrderType.MARKET,
        reference: str | None = None,
    ) -> list[str]:
        """Combo order with specified combo type and direction."""
        legs, combo_sig = self.engine.build_combo(option_data=option_data, combo_type=combo_type, direction=direction, volume=int(volume))
        return self._submit(
            legs=legs,
            direction=direction,
            price=price,
            volume=volume,
            order_type=order_type,
            combo_type=combo_type,
            combo_sig=combo_sig,
            reference=reference,
        )

    # =====================clsoe postion helper methods =======================
    def close_underlying_position(self) -> None:
        self.engine.position_engine.close_underlying_position(self, self.holding)

    def close_option_position(self, optionPosition: OptionPositionData) -> None:
        self.engine.position_engine.close_option_position(self, optionPosition)

    def close_all_option(self) -> None:
        self.engine.position_engine.close_all_option_positions(self, self.holding)

    def close_combo_position(self, comboPosition: ComboPositionData) -> None:
        self.engine.position_engine.close_combo_position(self, comboPosition)

    def close_all_combo_positions(self) -> None:
        self.engine.position_engine.close_all_combo_positions(self, self.holding)

    def close_all_strategy_positions(self) -> None:
        self.engine.position_engine.close_all_strategy_positions(self)

    def write_log(self, msg: str, level: int = INFO) -> None:
        # Write log to event system using strategy name as gateway_name
        log = LogData(msg=msg, level=level, gateway_name=self.strategy_name)
        event = Event(EVENT_LOG, log)
        self.engine.event_engine.put(event)

    def put_event(self) -> None:
        self.engine.put_strategy_event(self)

    def set_error(self, msg: str = "") -> None:
        self.error = True
        self.write_log(f"ERROR: {msg}")
        self.started = False
        self.put_event()
