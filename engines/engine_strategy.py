import importlib
import sys
import traceback
from collections import defaultdict
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import Lock
from types import ModuleType
from typing import TYPE_CHECKING

from engines.engine_log import DEBUG, ERROR, INFO, WARNING
from strategies.template import OptionStrategyTemplate, OptionStrategyTemplate as BaseTemplate
from utilities.base_engine import BaseEngine
from utilities.constant import ComboType, Direction, Exchange, OrderType, Status
from utilities.event import EVENT_LOG, EVENT_ORDER, EVENT_PORTFOLIO_STRATEGY, EVENT_TIMER, EVENT_TRADE
from utilities.object import CancelRequest, ContractData, Leg, LogData, OrderData, OrderRequest, StrategyHolding, TradeData
from utilities.portfolio import OptionData, PortfolioData
from utilities.utility import load_yaml_data, round_to, save_yaml

from .engine_comboBuilder import ComboBuilderEngine
from .engine_event import Event, EventEngine
from .engine_hedge import HedgeEngine
from .engine_position import PositionEngine

APP_NAME = "Strategy"

if TYPE_CHECKING:
    from engines.engine_main import MainEngine

BASE_PATH = Path(__file__).resolve().parent.parent
SETTINGS_PATH = BASE_PATH / "setting"
STRATEGY_PATH = BASE_PATH / "strategies"


class OptionStrategyEngine(BaseEngine):
    strategy_data_filename: str = str(SETTINGS_PATH.joinpath("strategy_data.yaml"))
    strategy_setting_filename: str = str(SETTINGS_PATH.joinpath("strategy_setting.yaml"))

    def __init__(self, main_engine: "MainEngine", event_engine: EventEngine) -> None:
        super().__init__(main_engine, event_engine, APP_NAME)

        self.position_engine = PositionEngine(self)
        self.hedge_engine = HedgeEngine(self)
        self.combo_builder_engine = ComboBuilderEngine(self)

        self.strategies: dict[str, OptionStrategyTemplate] = {}
        self.strategy_classes: dict[str, type[OptionStrategyTemplate]] = {}

        self.orderid_strategy_map: dict[str, OptionStrategyTemplate] = {}

        self.orders: dict[str, OrderData] = {}
        self.trades: dict[str, TradeData] = {}
        self.strategy_active_orders: dict[str, set[str]] = {}

        self.strategy_data: dict[str, dict] = {}
        self.strategy_setting: dict[str, dict] = {}

        self.init_executor: ThreadPoolExecutor = ThreadPoolExecutor(max_workers=1)
        self.strategy_locks: dict[str, Lock] = defaultdict(Lock)

        self.timer_count: int = 0
        self.timer_trigger: int = 60

        self.register_event()
        self.load_strategy_class()
        self.load_strategy_setting()
        self.load_strategy_data()

    def register_event(self) -> None:
        # Register event handlers for strategy-specific events
        self.event_engine.register(EVENT_ORDER, self.process_order_event)
        self.event_engine.register(EVENT_TRADE, self.process_trade_event)
        self.event_engine.register(EVENT_TIMER, self.process_timer_event)

    # ---------------- Event Handling ---------------------------

    def process_order_event(self, event: Event) -> None:
        order: OrderData = event.data

        strategy: OptionStrategyTemplate | None = self.orderid_strategy_map.get(order.orderid)
        if not strategy:
            return

        self.orders[order.orderid] = order

        self.main_engine.save_order_data(strategy.strategy_name, order)

        self.position_engine.process_order(order)

        self.call_strategy_func(strategy, strategy.on_order, order)

        if order.status in {Status.CANCELLED, Status.REJECTED}:
            active_set = self.strategy_active_orders.get(strategy.strategy_name)
            if active_set and order.orderid in active_set:
                active_set.discard(order.orderid)
            self.orderid_strategy_map.pop(order.orderid, None)
        elif order.status == Status.ALLTRADED:
            active_set = self.strategy_active_orders.get(strategy.strategy_name)
            if active_set and order.orderid in active_set:
                active_set.discard(order.orderid)

    def process_trade_event(self, event: Event) -> None:
        trade: TradeData = event.data

        strategy: OptionStrategyTemplate | None = self.orderid_strategy_map.get(trade.orderid)
        if not strategy:
            return

        self.trades[trade.tradeid] = trade

        self.main_engine.save_trade_data(strategy.strategy_name, trade)

        self.position_engine.process_trade(strategy.strategy_name, trade)

        self.call_strategy_func(strategy, strategy.on_trade, trade)

    def process_timer_event(self, event: Event) -> None:
        self.timer_count += 1

        if self.timer_count >= self.timer_trigger:
            self.timer_count = 0

        for strategy in self.strategies.values():
            if strategy.inited and strategy.started and not getattr(strategy, "error", False):
                try:
                    strategy.on_timer()
                except Exception as e:
                    strategy.set_error(str(e))
                    self.write_log(f"{strategy.strategy_name} on_timer exception:\n{traceback.format_exc()}", ERROR)

    def get_contract(self, symbol: str) -> ContractData | None:
        return self.main_engine.get_contract(symbol)

    # ---------------- Exposed Methods --------------------------

    def get_strategy_holding(self, strategy_name: str) -> StrategyHolding | None:
        """Get holding data for a specific strategy"""
        return self.position_engine.get_holding(strategy_name)

    def get_strategy(self, strategy_name: str) -> OptionStrategyTemplate | None:
        """Safely get a strategy instance by name."""
        return self.strategies.get(strategy_name)

    def get_portfolio(self, portfolio_name: str) -> PortfolioData | None:
        return self.main_engine.get_portfolio(portfolio_name)

    def build_combo(
        self, option_data: dict[str, "OptionData"], combo_type: ComboType, direction: Direction, volume: int
    ) -> tuple[list[Leg], str]:
        """Build combo from option data by delegating to ComboBuilderEngine."""
        return self.combo_builder_engine.combo_builder(option_data=option_data, combo_type=combo_type, direction=direction, volume=volume)

    # ---------------- Strategy Lifecycle Control -----------------

    def call_strategy_func(self, strategy: OptionStrategyTemplate, func: Callable, params: object = None) -> bool:
        # Safely call strategy function with error handling
        try:
            if params:
                func(params)
            else:
                func()
            return True
        except Exception:
            msg: str = f"Exception triggered:\n{traceback.format_exc()}"
            self.write_log(f"Strategy {strategy.strategy_name} exception: {msg}", ERROR)
            return False

    def add_strategy(self, class_name: str, portfolio_name: str, setting: dict) -> None:
        """Add new strategy instance with auto-recovery support"""
        strategy_name = f"{class_name}_{portfolio_name}"

        if strategy_name in self.strategies:
            self.write_log("Strategy creation failed, strategy already exists for this portfolio", WARNING)
            return

        if strategy_name in self.strategy_setting:
            self.write_log(f"Found removed strategy {strategy_name}, auto-recovering...", INFO)
            self.recover_strategy(strategy_name)
            return

        strategy_class: type[OptionStrategyTemplate] | None = self.strategy_classes.get(class_name, None)
        if not strategy_class:
            self.write_log(f"Strategy creation failed, strategy class {class_name} not found", ERROR)
            return

        strategy: OptionStrategyTemplate = strategy_class(self, strategy_name, portfolio_name, setting)
        self.strategies[strategy_name] = strategy

        # Ensure holding for this strategy
        portfolio = self.get_portfolio(portfolio_name)
        if portfolio:
            self.position_engine.get_create_strategy_holding(strategy_name)
            holding = self.position_engine.get_holding(strategy_name)
            strategy.holding = holding

        self.save_strategy_setting()
        self.put_strategy_event(strategy)

        self.write_log(f"Successfully created new strategy {strategy_name}", INFO)

    def recover_strategy(self, strategy_name: str) -> bool:
        if strategy_name not in self.strategy_setting:
            self.write_log(f"Strategy {strategy_name} configuration not found, cannot recover", ERROR)
            return False

        config = self.strategy_setting[strategy_name]
        strategy_class = self.strategy_classes.get(config["class_name"])
        if not strategy_class:
            self.write_log(f"Strategy recovery failed, class {config['class_name']} not found", ERROR)
            return False

        strategy = strategy_class(self, strategy_name, config["portfolio_name"], config["setting"])
        self.strategies[strategy_name] = strategy

        portfolio = self.get_portfolio(config["portfolio_name"])
        if portfolio is None:
            self.write_log(f"Strategy recovery failed, portfolio {config['portfolio_name']} not found", ERROR)
            return False

        self.position_engine.get_create_strategy_holding(strategy_name)

        holding_data = self.strategy_data.get(strategy_name)
        if holding_data:
            self.position_engine.load_serialized_holding(strategy_name, holding_data)

        strategy.holding = self.position_engine.get_holding(strategy_name)

        self.put_strategy_event(strategy)
        self.write_log(f"Strategy {strategy_name} recovered with all data", INFO)
        return True

    def init_strategy(self, strategy_name: str) -> None:
        self.init_executor.submit(self._init_strategy, strategy_name)

    def _init_strategy(self, strategy_name: str) -> None:
        strategy = self.strategies[strategy_name]

        if strategy.inited:
            self.write_log(f"{strategy_name} already initialized, duplicate operation not allowed", WARNING)
            return

        self.call_strategy_func(strategy, strategy.on_init)
        self.put_strategy_event(strategy)

    def start_strategy(self, strategy_name: str) -> None:
        strategy: OptionStrategyTemplate = self.strategies[strategy_name]
        if not strategy.inited:
            self.write_log(f"Strategy {strategy.strategy_name} start failed, please initialize first", ERROR)
            return

        if strategy.started:
            self.write_log(f"{strategy_name} already started, duplicate operation not allowed", WARNING)
            return

        self.call_strategy_func(strategy, strategy.on_start)
        self.put_strategy_event(strategy)

    def stop_strategy(self, strategy_name: str) -> None:
        strategy: OptionStrategyTemplate = self.strategies[strategy_name]
        if not strategy.started:
            self.write_log(f"{strategy_name} already stopped, duplicate operation not allowed", WARNING)
            return

        self.call_strategy_func(strategy, strategy.on_stop)
        self.cancel_all(strategy)

        self.sync_strategy_data(strategy)
        self.put_strategy_event(strategy)

    def remove_strategy(self, strategy_name: str) -> bool:
        strategy: OptionStrategyTemplate = self.strategies[strategy_name]
        if strategy.started:
            self.write_log(f"Strategy {strategy.strategy_name} removal failed, please stop first", ERROR)
            return False

        self.sync_strategy_data(strategy)

        # Clean up any outstanding order mapping for this strategy
        active_ids = self.strategy_active_orders.get(strategy.strategy_name, set())
        for orderid in list(active_ids):
            if orderid in self.orderid_strategy_map:
                self.orderid_strategy_map.pop(orderid)

        self.main_engine.unsubscribe_chains(strategy_name)

        self.strategies.pop(strategy_name)
        self.position_engine.remove_strategy_holding(strategy_name)  # Clean up holding data

        self.write_log(f"Strategy {strategy_name} removed, data saved to file", INFO)
        return True

    def delete_strategy(self, strategy_name: str) -> bool:
        strategy: OptionStrategyTemplate = self.strategies[strategy_name]
        if strategy.started:
            self.write_log(f"Strategy {strategy.strategy_name} deletion failed, please stop first", ERROR)
            return False

        active_ids = self.strategy_active_orders.get(strategy.strategy_name, set())
        for orderid in list(active_ids):
            if orderid in self.orderid_strategy_map:
                self.orderid_strategy_map.pop(orderid)

        self.strategies.pop(strategy_name)
        self.position_engine.remove_strategy_holding(strategy_name)  # Clean up holding data
        self.strategy_setting.pop(strategy_name, None)
        self.strategy_data.pop(strategy_name, None)

        self.save_strategy_setting()
        self.save_strategy_data()

        self.write_log(f"Strategy {strategy_name} completely deleted, needs to be recreated", INFO)
        return True

    def close_strategy_positions(self, strategy_name: str) -> None:
        """Delegate to PositionEngine to clear all positions for a strategy."""
        strategy = self.strategies.get(strategy_name)
        if not strategy:
            self.write_log(f"Strategy {strategy_name} not found", WARNING)
            return
        self.position_engine.close_all_strategy_positions(strategy)

    # ---------------- Data File Operations ---------------------

    def load_strategy_data(self) -> None:
        """Load strategy data from YAML file."""
        self.strategy_data = load_yaml_data(self.strategy_data_filename)

    def save_strategy_data(self) -> None:
        """Save strategy data to YAML file - preserves existing data, only updates current strategies."""
        existing_data = load_yaml_data(self.strategy_data_filename)

        for strategy_name, _ in self.strategies.items():
            serialized = self.position_engine.serialize_holding(strategy_name)
            if serialized is not None:
                existing_data[strategy_name] = serialized

        self.strategy_data = existing_data
        save_yaml(self.strategy_data_filename, existing_data)

    def sync_strategy_data(self, strategy: OptionStrategyTemplate) -> None:
        """Sync strategy data to YAML file - preserves existing data, only updates current strategy."""
        strategy_name = strategy.strategy_name
        serialized = self.position_engine.serialize_holding(strategy_name)

        existing_data = load_yaml_data(self.strategy_data_filename)
        existing_data[strategy_name] = serialized

        self.strategy_data[strategy_name] = serialized
        save_yaml(self.strategy_data_filename, existing_data)

    def load_strategy_setting(self) -> None:
        """Load strategy settings from YAML file."""
        strategy_setting: dict = load_yaml_data(self.strategy_setting_filename)

        self.strategy_setting = strategy_setting
        self.write_log(f"Loaded {len(strategy_setting)} strategy configurations to history", INFO)

    def save_strategy_setting(self) -> None:
        """Save strategy settings to YAML file - only updates current strategies, preserves others."""
        existing_data = load_yaml_data(self.strategy_setting_filename)

        for name, strategy in self.strategies.items():
            existing_data[name] = {
                "class_name": strategy.__class__.__name__,
                "setting": strategy.get_parameters(),
                "portfolio_name": strategy.portfolio_name,
            }

        self.strategy_setting = existing_data
        save_yaml(self.strategy_setting_filename, existing_data)

    # ---------------- Strategy Information Query ----------------

    def get_strategy_class_parameters(self, class_name: str) -> dict:
        # Get strategy class parameters
        strategy_class: type[OptionStrategyTemplate] = self.strategy_classes[class_name]

        parameters: dict = {}
        for name in strategy_class.parameters:
            parameters[name] = getattr(strategy_class, name)

        return parameters

    def get_strategy_parameters(self, strategy_name: str) -> dict:
        # Get strategy instance parameters
        strategy: OptionStrategyTemplate = self.strategies[strategy_name]
        return strategy.get_parameters()

    def get_available_strategies(self) -> dict:
        # Get available strategy lists
        memory_strategies = list(self.strategies.keys())
        history_strategies = list(self.strategy_setting.keys())

        removed_strategies = [name for name in history_strategies if name not in memory_strategies]

        return {"memory_strategies": memory_strategies, "history_strategies": history_strategies, "removed_strategies": removed_strategies}

    def get_removed_strategies(self) -> list[str]:
        # Get recoverable strategy list
        all_strategy_names = list(self.strategy_setting.keys())
        memory_strategies = list(self.strategies.keys())
        return [name for name in all_strategy_names if name not in memory_strategies]

    def get_all_strategy_class_names(self) -> list:
        # Get all loaded strategy class names
        return list(self.strategy_classes.keys())

    # ---------------- OMS cache queries (moved from MainEngine) ---------------

    def get_order(self, orderid: str) -> OrderData | None:
        return self.orders.get(orderid)

    def get_trade(self, tradeid: str) -> TradeData | None:
        return self.trades.get(tradeid)

    def get_all_orders(self) -> list[OrderData]:
        return list(self.orders.values())

    def get_all_trades(self) -> list[TradeData]:
        return list(self.trades.values())

    def get_all_active_orders(self) -> list[OrderData]:
        result: list[OrderData] = []
        for _, order_ids in self.strategy_active_orders.items():
            for order_id in order_ids:
                order_obj = self.orders.get(order_id)
                if order_obj and order_obj.is_active():
                    result.append(order_obj)
        return result

    def put_strategy_event(self, strategy: OptionStrategyTemplate) -> None:
        # Emit strategy event for UI updates
        data: dict = {
            "strategy_name": strategy.strategy_name,
            "class_name": strategy.__class__.__name__,
            "author": strategy.author,
            "parameters": strategy.get_parameters(),
            "variables": strategy.get_variables(),
        }

        serialized = self.position_engine.serialize_holding(strategy.strategy_name)
        if serialized:
            variables: dict = dict(data["variables"])
            variables["holding"] = serialized
            data["variables"] = variables

        event = Event(EVENT_PORTFOLIO_STRATEGY, data)
        self.event_engine.put(event)

    # ---------------- Strategy Loading and Registration ---------

    def load_strategy_class(self) -> None:
        # Load strategy classes from strategies folder
        ROOT_PATH = Path(__file__).resolve().parent.parent

        if str(ROOT_PATH) not in sys.path:
            sys.path.insert(0, str(ROOT_PATH))

        strategy_path: Path = ROOT_PATH.joinpath("strategies")
        self.load_strategy_class_from_folder(strategy_path, "strategies")

        strategy_count = len(self.strategy_classes)
        self.write_log(f"Loaded {strategy_count} strategy classes", INFO)

    def load_strategy_class_from_folder(self, path: Path, module_name: str = "") -> None:
        # Load strategy classes from specified folder
        if not path.exists():
            self.write_log(f"Strategy folder does not exist: {path}", WARNING)
            return

        # Recursively discover modules/files under the folder (supports subpackages)
        discovered_files: list[Path] = [p for suffix in ["py", "pyd", "so"] for p in path.rglob(f"*.{suffix}")]

        for file_path in discovered_files:
            if file_path.name.startswith("__"):
                continue

            rel_path = file_path.relative_to(path)

            if any(part.startswith("__") for part in rel_path.parts):
                continue

            module_rel = ".".join(rel_path.with_suffix("").parts)
            module_path: str = f"{module_name}.{module_rel}" if module_name else module_rel
            self.load_strategy_class_from_module(module_path)

    def load_strategy_class_from_module(self, module_name: str) -> None:
        # Load strategy classes from module
        try:
            module: ModuleType = importlib.import_module(module_name)

            for name in dir(module):
                value = getattr(module, name)
                if isinstance(value, type):
                    try:
                        if issubclass(value, BaseTemplate) and value.__name__ != "OptionStrategyTemplate":
                            self.strategy_classes[value.__name__] = value
                    except TypeError:
                        continue

        except Exception:
            msg: str = f"Failed to load strategy file {module_name}. Error:\n{traceback.format_exc()}"
            self.write_log(msg, ERROR)

    # ---------------- Order Related ----------------------------

    def send_order(
        self,
        strategy: OptionStrategyTemplate,
        symbol: str,
        direction: Direction,
        price: float,
        volume: float,
        combo_type: ComboType | None = None,
        combo_sig: str | None = None,
        order_type: OrderType = OrderType.LIMIT,
        legs: list[Leg] | None = None,
        reference: str | None = None,
        trading_class: str | None = None,
    ) -> list[str]:
        # Send order from strategy (supports limit, market, combo orders)
        reference = reference or f"{APP_NAME}_{strategy.strategy_name}"

        if legs:
            trading_class = legs[0].trading_class
            type_part = combo_type.value if combo_type else ""
            sig_part = f"{combo_sig}" if combo_sig else ""
            symbol = f"{strategy.portfolio_name}_{type_part}_{sig_part}"

            final_price = 0.0 if order_type == OrderType.MARKET else round_to(price, 0.01)

            req = OrderRequest(
                symbol=symbol,
                exchange=Exchange.SMART,
                direction=direction,
                type=order_type,
                volume=volume,
                price=final_price,
                combo_type=combo_type,
                legs=legs,
                reference=reference,
                trading_class=trading_class,
                is_combo=True,
            )
            self.write_log(f"Send combo order: {order_type}, price={final_price}, symbol={symbol}", DEBUG)

        else:
            contract = self.main_engine.get_contract(symbol)
            if not contract:
                self.write_log(f"Order failed: contract {symbol} not found", ERROR)
                return []

            final_price = 0.0 if order_type == OrderType.MARKET else round_to(price, 0.01)

            req = OrderRequest(
                symbol=contract.symbol,
                exchange=Exchange.SMART,
                direction=direction,
                type=order_type,
                volume=round_to(volume, contract.min_volume),
                price=final_price,
                reference=reference,
                trading_class=contract.trading_class,
            )
            self.write_log(f"Send regular order: {order_type}, price={final_price}, symbol={contract.symbol}", DEBUG)

        orderid = self.main_engine.send_order(req)
        if not orderid:
            return []

        self.orderid_strategy_map[orderid] = strategy

        if strategy.strategy_name not in self.strategy_active_orders:
            self.strategy_active_orders[strategy.strategy_name] = set()

        self.strategy_active_orders[strategy.strategy_name].add(orderid)

        return [orderid]

    def cancel_order(self, strategy: OptionStrategyTemplate, orderid: str) -> None:
        # Cancel specific order, supports combo orders
        order: OrderData | None = self.orders.get(orderid)
        if order:
            req: CancelRequest = order.create_cancel_request()

            if order.is_combo:
                req.is_combo = True
                req.legs = order.legs

            self.main_engine.cancel_order(req)

    def cancel_all(self, strategy: OptionStrategyTemplate) -> None:
        # Cancel all active orders for strategy
        active_set = self.strategy_active_orders.get(strategy.strategy_name, set())
        if active_set:
            for orderid in list(active_set):
                self.cancel_order(strategy, orderid)

    # ---------------- Strategy Option Chain Registration and Query -----------

    def subscribe_chains(self, strategy_name: str, chain_symbols: list[str]) -> None:
        """Subscribe strategy to chain updates"""
        self.main_engine.subscribe_chains(strategy_name, chain_symbols)

    def unsubscribe_chains(self, strategy_name: str) -> None:
        """Unsubscribe strategy from all chains"""
        self.main_engine.unsubscribe_chains(strategy_name)

    # ---------------- Utility Methods --------------------------

    def set_timer_trigger(self, timer_trigger: int) -> None:
        # Set timer trigger interval
        self.timer_trigger = timer_trigger

    def write_log(self, msg: str, level: int = INFO) -> None:
        # Write log to event system
        log = LogData(msg=msg, level=level, gateway_name=APP_NAME)
        event = Event(EVENT_LOG, log)
        self.event_engine.put(event)

    def stop_all_strategies(self) -> None:
        """Stop all running strategies."""
        for strategy_name in list(self.strategies.keys()):
            strategy = self.strategies[strategy_name]
            if strategy.started:
                self.stop_strategy(strategy_name)

    def close(self) -> None:
        """Close engine and save all data."""
        self.save_strategy_setting()
        self.save_strategy_data()
        self.stop_all_strategies()
