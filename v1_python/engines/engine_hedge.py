from typing import TYPE_CHECKING

from engines.engine_event import Event, EventEngine
from engines.engine_log import INFO
from strategies.template import OptionStrategyTemplate
from utilities.constant import Direction, OrderType
from utilities.event import EVENT_LOG, EVENT_TIMER
from utilities.object import ContractData, LogData
from utilities.portfolio import UnderlyingData

APP_NAME = "Hedge"

if TYPE_CHECKING:
    from engines.engine_strategy import OptionStrategyEngine


class HedgeConfig:
    """Configuration for strategy hedging"""

    def __init__(self, strategy: OptionStrategyTemplate, timer_trigger: int = 5, delta_target: int = 0, delta_range: int = 0):
        self.strategy = strategy
        self.strategy_name = strategy.strategy_name
        self.timer_trigger = timer_trigger
        self.delta_target = delta_target
        self.delta_range = delta_range


class HedgeEngine:
    """Centralized hedge engine that manages hedging for multiple strategies"""

    def __init__(self, option_strategy_engine: "OptionStrategyEngine") -> None:
        self.option_strategy_engine: "OptionStrategyEngine" = option_strategy_engine
        self.event_engine: EventEngine = option_strategy_engine.event_engine

        # Strategy registration system
        self.registered_strategies: dict[str, HedgeConfig] = {}
        self.timer_count: int = 0
        self.timer_trigger: int = 5

        self.register_event()

    def register_event(self) -> None:
        self.event_engine.register(EVENT_TIMER, self.process_timer_event)

    def register_strategy(
        self, strategy: OptionStrategyTemplate, timer_trigger: int = 5, delta_target: int = 0, delta_range: int = 0
    ) -> None:
        # Validate strategy has underlying symbol
        if not strategy.underlying or not strategy.underlying.symbol:
            self.write_log(f"Cannot register strategy {strategy.strategy_name}: no underlying symbol", INFO)
            return

        config = HedgeConfig(strategy=strategy, timer_trigger=timer_trigger, delta_target=delta_target, delta_range=delta_range)

        self.registered_strategies[strategy.strategy_name] = config
        self.write_log(f"Strategy {strategy.strategy_name} registered for hedging", INFO)

    def unregister_strategy(self, strategy_name: str) -> None:
        if strategy_name in self.registered_strategies:
            del self.registered_strategies[strategy_name]
            self.write_log(f"Strategy {strategy_name} unregistered from hedging", INFO)

    def write_log(self, msg: str, level: int = INFO) -> None:
        # Write log to event system
        log = LogData(msg=msg, level=level, gateway_name=APP_NAME)
        event = Event(EVENT_LOG, log)
        self.event_engine.put(event)

    def process_timer_event(self, event: Event) -> None:
        self.timer_count += 1
        if self.timer_count < self.timer_trigger:
            return
        self.timer_count = 0

        for strategy_name, config in self.registered_strategies.items():
            self.run_strategy_hedging(strategy_name, config)

    def run_strategy_hedging(self, strategy_name: str, config: HedgeConfig) -> None:
        if not self.check_strategy_orders_finished(strategy_name):
            self.cancel_strategy_orders(strategy_name)
            return

        plan = self.compute_hedge_plan(strategy_name, config)
        if not plan:
            return

        symbol, direction, available, order_volume = plan
        self.execute_hedge_orders(strategy_name, symbol, direction, available, order_volume)

    def compute_hedge_plan(self, strategy_name: str, config: HedgeConfig) -> tuple[str, Direction, float, float] | None:
        strategy = self.option_strategy_engine.get_strategy(strategy_name)
        if not strategy:
            return None

        holding = strategy.holding

        total_delta = holding.summary.delta
        delta_max = config.delta_target + config.delta_range
        delta_min = config.delta_target - config.delta_range
        if delta_min <= total_delta <= delta_max:
            return None

        delta_to_hedge = config.delta_target - total_delta
        portfolio = strategy.portfolio
        if portfolio is None or not portfolio.underlying:
            return None

        underlying: UnderlyingData = portfolio.underlying
        hedge_volume = delta_to_hedge / underlying.theo_delta
        symbol = underlying.symbol

        contract: ContractData | None = self.option_strategy_engine.get_contract(symbol)
        if not contract:
            return None
        if abs(hedge_volume) < 1:
            return None

        # Determine direction and available close quantity from strategy holding's underlying
        qty = holding.underlyingPosition.quantity
        if hedge_volume > 0:
            direction = Direction.LONG
            available = abs(qty) if qty < 0 else 0
        else:
            direction = Direction.SHORT
            available = qty if qty > 0 else 0

        return symbol, direction, float(available), float(abs(hedge_volume))

    def execute_hedge_orders(self, strategy_name: str, symbol: str, direction: Direction, available: float, order_volume: float) -> None:
        strategy = self.option_strategy_engine.get_strategy(strategy_name)
        if not strategy:
            return

        remaining = order_volume

        # First CLOSE existing position if available
        if available > 0:
            close_vol = min(available, order_volume)
            self.submit_hedge_order(strategy, symbol, direction, close_vol)
            remaining -= close_vol

        # Then OPEN new hedge if needed
        if remaining > 0:
            self.submit_hedge_order(strategy, symbol, direction, remaining)

    def submit_hedge_order(self, strategy: OptionStrategyTemplate, symbol: str, direction: Direction, volume: float) -> None:
        # Use strategy's underlying order helper methods
        strategy.underlying_order(
            direction=direction, price=0.0, volume=volume, order_type=OrderType.MARKET, reference=f"Hedge_{strategy.strategy_name}"
        )

        self.write_log(f"Hedge sending order: dir={direction.name}, vol={volume}, symbol={symbol}", INFO)

    def check_strategy_orders_finished(self, strategy_name: str) -> bool:
        # Check if strategy has any hedge orders (orders with APP_NAME in reference)
        active_orderids = self.option_strategy_engine.strategy_active_orders.get(strategy_name, set())
        hedge_orderids: set[str] = set()

        for orderid in active_orderids:
            order = self.option_strategy_engine.get_order(orderid)
            if order and order.reference and APP_NAME in order.reference:
                hedge_orderids.add(orderid)

        return len(hedge_orderids) == 0

    def cancel_strategy_orders(self, strategy_name: str) -> None:
        # Use strategy engine's active orders map
        active_orderids = self.option_strategy_engine.strategy_active_orders.get(strategy_name, set())
        hedge_orderids: list[str] = []

        for orderid in active_orderids:
            existing_order = self.option_strategy_engine.get_order(orderid)
            if existing_order and existing_order.reference and APP_NAME in existing_order.reference:
                hedge_orderids.append(orderid)

        # Get strategy instance to pass to cancel_order
        strategy = self.option_strategy_engine.get_strategy(strategy_name)
        if not strategy:
            return

        for orderid in hedge_orderids:
            self.option_strategy_engine.cancel_order(strategy, orderid)
