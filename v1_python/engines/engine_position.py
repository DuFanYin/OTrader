from __future__ import annotations

from typing import TYPE_CHECKING, Any

from engines.engine_log import ERROR, INFO
from utilities.constant import ComboType, Direction, OrderType
from utilities.event import EVENT_TIMER
from utilities.object import (
    BasePosition,
    ComboPositionData,
    OptionPositionData,
    OrderData,
    PortfolioSummary,
    StrategyHolding,
    TradeData,
    UnderlyingPositionData,
)
from utilities.portfolio import PortfolioData

if TYPE_CHECKING:
    from engines.engine_event import Event
    from engines.engine_strategy import OptionStrategyEngine
    from strategies.template import OptionStrategyTemplate
else:
    from engines.engine_event import Event

APP_NAME = "PositionEngine"


class PositionEngine:
    def __init__(self, option_strategy_engine: OptionStrategyEngine) -> None:
        self.option_strategy_engine = option_strategy_engine
        self.main_engine = option_strategy_engine.main_engine
        self.event_engine = option_strategy_engine.event_engine

        self.strategy_holdings: dict[str, StrategyHolding] = {}

        self.order_meta: dict[str, dict] = {}
        self.trade_seen: set[str] = set()

        self.register_event()

    def register_event(self) -> None:
        self.event_engine.register(EVENT_TIMER, self.process_timer_event)

    def process_timer_event(self, event: Event) -> None:
        for strategy_name in self.strategy_holdings.keys():
            try:
                self.update_metrics(strategy_name)
            except Exception as e:
                self.write_log(f"[PositionEngine] Metrics update error: {e}", ERROR)

    def write_log(self, msg: str, level: int = INFO) -> None:
        self.option_strategy_engine.write_log(msg, level)

    # ====================== STRATEGY INIT ======================
    def get_create_strategy_holding(self, strategy_name: str) -> None:
        if strategy_name not in self.strategy_holdings:
            self.strategy_holdings[strategy_name] = StrategyHolding()

    def remove_strategy_holding(self, strategy_name: str) -> None:
        self.strategy_holdings.pop(strategy_name, None)

    def get_holding(self, strategy_name: str) -> StrategyHolding:
        return self.strategy_holdings[strategy_name]

    # ====================== Process Order and Trade ======================
    def process_order(self, order: OrderData) -> None:
        legs_meta = None
        if order.is_combo and order.legs:
            legs_meta = []
            for leg in order.legs:
                legs_meta.append({"symbol": leg.symbol, "con_id": leg.con_id, "ratio": leg.ratio, "direction": leg.direction})

        self.order_meta[order.orderid] = {
            "is_combo": order.is_combo,
            "symbol": order.symbol,
            "combo_type": (order.combo_type.name if order.combo_type else None),
            "legs": legs_meta,
        }

    def process_trade(self, strategy_name: str, trade: TradeData) -> None:
        if trade.tradeid in self.trade_seen:
            return
        self.trade_seen.add(trade.tradeid)

        holding = self.strategy_holdings[strategy_name]
        meta = self.order_meta.get(trade.orderid)

        if meta and meta.get("is_combo"):
            combo_symbol = meta["symbol"]
            combo_type = meta.get("combo_type")
            legs_meta = meta.get("legs")

            if combo_type:
                combo_type = ComboType[combo_type]
            else:
                combo_type = ComboType.CUSTOM

            combo = self.get_or_create_combo_position(holding, combo_symbol, combo_type, legs_meta)

            if trade.symbol == combo_symbol:
                self.apply_position_change(combo, trade)
            else:
                leg = self.get_or_create_option_position(combo, trade)
                self.apply_position_change(leg, trade)
            return

        elif trade.symbol.endswith(".STK"):
            self.apply_underlying_trade(holding, trade)
            return

        else:
            self.apply_single_leg_option_trade(holding, trade)

    # ====================== UNDERLYING ======================
    def apply_underlying_trade(self, holding: StrategyHolding, trade: TradeData) -> None:
        pos = holding.underlyingPosition
        if not pos.symbol:
            pos.symbol = trade.symbol
        self.apply_position_change(pos, trade)

    def apply_single_leg_option_trade(self, holding: StrategyHolding, trade: TradeData) -> None:
        pos = holding.optionPositions.get(trade.symbol)
        if not pos:
            pos = OptionPositionData(symbol=trade.symbol)
            holding.optionPositions[trade.symbol] = pos
        self.apply_position_change(pos, trade)

    # ====================== COMBO ======================
    def get_or_create_combo_position(
        self, holding: StrategyHolding, symbol: str, combo_type: ComboType, legs_meta: list[dict] | None
    ) -> ComboPositionData:
        combo = holding.comboPositions.get(symbol)
        if combo:
            return combo

        normalized_symbol = self._normalize_combo_symbol(symbol)
        for existing_symbol, existing_combo in holding.comboPositions.items():
            if self._normalize_combo_symbol(existing_symbol) == normalized_symbol:
                return existing_combo

        combo = ComboPositionData(symbol=symbol, combo_type=combo_type)

        if legs_meta:
            for m in legs_meta:
                combo.legs.append(OptionPositionData(symbol=m.get("symbol", "")))

        holding.comboPositions[symbol] = combo
        return combo

    def get_or_create_option_position(self, combo: ComboPositionData, trade: TradeData) -> OptionPositionData:
        for leg in combo.legs:
            if leg.symbol == trade.symbol:
                return leg
        new_leg = OptionPositionData(symbol=trade.symbol)
        combo.legs.append(new_leg)
        return new_leg

    # ───────────────────────────────────────────
    # Shared position mutation
    # ───────────────────────────────────────────
    def apply_position_change(self, pos: BasePosition, trade: TradeData) -> None:
        qty = abs(int(trade.volume))
        signed = qty if trade.direction == Direction.LONG else -qty
        prev_qty = pos.quantity
        multiplier = pos.multiplier

        if isinstance(pos, ComboPositionData):
            pos.quantity += signed
            pos.cost_value = round(pos.avg_cost * abs(pos.quantity) * multiplier, 2)
            return

        # --- Case 1: Same direction or new open ---
        if prev_qty == 0 or (prev_qty > 0 and signed > 0) or (prev_qty < 0 and signed < 0):
            total_qty = abs(prev_qty) + qty
            if prev_qty == 0:
                pos.avg_cost = round(trade.price, 2)
            else:
                pos.avg_cost = round((pos.avg_cost * abs(prev_qty) + trade.price * qty) / total_qty, 2)
            pos.quantity += signed
            pos.cost_value = round(pos.avg_cost * abs(pos.quantity) * multiplier, 2)
            return

        # --- Case 2: Opposite direction → closing trade ---
        close_qty = min(abs(prev_qty), qty)

        if prev_qty > 0:
            pnl = round((trade.price - pos.avg_cost) * close_qty, 2)
        else:
            pnl = round((pos.avg_cost - trade.price) * close_qty, 2)

        pos.realized_pnl += round(pnl * multiplier, 2)

        new_qty = abs(prev_qty) - close_qty
        if new_qty == 0:
            pos.quantity = 0
            pos.avg_cost = 0.0
            pos.cost_value = 0.0
        else:
            pos.quantity = (1 if prev_qty > 0 else -1) * new_qty
            pos.cost_value = round(pos.avg_cost * abs(pos.quantity) * multiplier, 2)

        # --- Case 3: Reverse position ---
        extra = qty - close_qty
        if extra > 0:
            pos.avg_cost = round(trade.price, 2)
            pos.quantity = (1 if signed > 0 else -1) * extra
            pos.cost_value = round(pos.avg_cost * abs(pos.quantity) * multiplier, 2)

    # ========================================================================
    # METRICS AND CALCULATIONS
    # ========================================================================

    def update_metrics(self, strategy_name: str) -> None:
        """Update portfolio metrics for a strategy."""
        holding = self.strategy_holdings[strategy_name]

        portfolio_name = strategy_name.split("_", 1)[-1] if "_" in strategy_name else strategy_name
        portfolio = self.option_strategy_engine.get_portfolio(portfolio_name)
        if not portfolio:
            return

        totals = {"cv": 0.0, "tc": 0.0, "rlz": 0.0, "delta": 0.0, "gamma": 0.0, "theta": 0.0, "vega": 0.0}

        for option_position in holding.optionPositions.values():
            option_data = portfolio.options.get(option_position.symbol)
            self._add_totals(totals, self._accumulate_position(option_position, option_data))

        underlying_position = holding.underlyingPosition
        if underlying_position.quantity or underlying_position.realized_pnl:
            self._add_totals(totals, self._accumulate_position(underlying_position, portfolio.underlying))

        for combo_position in holding.comboPositions.values():
            self._add_totals(totals, self._accumulate_combo_position(combo_position, portfolio))

        unreal = totals["cv"] - totals["tc"]
        s = holding.summary
        s.current_value = round(totals["cv"], 2)
        s.total_cost = round(totals["tc"], 2)
        s.unrealized_pnl = round(unreal, 2)
        s.realized_pnl = round(totals["rlz"], 2)
        s.pnl = round(unreal + totals["rlz"], 2)
        s.delta = round(totals["delta"], 4)
        s.gamma = round(totals["gamma"], 4)
        s.theta = round(totals["theta"], 4)
        s.vega = round(totals["vega"], 4)

        for option_position in holding.optionPositions.values():
            option_position.clear_fields()

        holding.underlyingPosition.clear_fields()

        for combo_position in holding.comboPositions.values():
            combo_position.clear_fields()

    def _accumulate_position(self, position: BasePosition, position_snapshot: Any | None) -> dict[str, float]:
        """Accumulate metrics for a single position using instrument data."""

        if position_snapshot:
            position.delta = round(float(getattr(position_snapshot, "delta", 0.0)), 4)
            position.gamma = round(float(getattr(position_snapshot, "gamma", 0.0)), 4)
            position.theta = round(float(getattr(position_snapshot, "theta", 0.0)), 4)
            position.vega = round(float(getattr(position_snapshot, "vega", 0.0)), 4)
            position.mid_price = round(float(getattr(position_snapshot, "mid_price", 0.0)), 2)

        return {
            "cv": round(position.current_value, 2),
            "tc": round(position.cost_value, 2),
            "rlz": round(position.realized_pnl, 2),
            "delta": round(position.quantity * position.delta, 4),
            "gamma": round(position.quantity * position.gamma, 4),
            "theta": round(position.quantity * position.theta, 4),
            "vega": round(position.quantity * position.vega, 4),
        }

    def _accumulate_combo_position(self, combo: ComboPositionData, portfolio: PortfolioData) -> dict:
        """Accumulate metrics for a combo position."""
        combo.delta = combo.gamma = combo.theta = combo.vega = 0.0
        combo.cost_value = 0.0
        combo.realized_pnl = 0.0
        current_value = 0.0

        for leg in combo.legs:
            instrument_data = portfolio.options.get(leg.symbol)
            m = self._accumulate_position(leg, instrument_data)

            current_value += m["cv"]
            combo.cost_value += m["tc"]
            combo.realized_pnl += m["rlz"]
            combo.delta += m["delta"]
            combo.gamma += m["gamma"]
            combo.theta += m["theta"]
            combo.vega += m["vega"]

        if combo.quantity != 0:
            combo.mid_price = round(current_value / (abs(combo.quantity) * combo.multiplier), 2)
            if combo.cost_value > 0:
                combo.avg_cost = round(combo.cost_value / (abs(combo.quantity) * combo.multiplier), 2)

        return {
            "cv": round(current_value, 2),
            "tc": round(combo.cost_value, 2),
            "rlz": round(combo.realized_pnl, 2),
            "delta": round(combo.delta, 4),
            "gamma": round(combo.gamma, 4),
            "theta": round(combo.theta, 4),
            "vega": round(combo.vega, 4),
        }

    # ========================================================================
    # POSITION CLEARING OPERATIONS
    # ========================================================================

    def close_all_strategy_positions(self, strategy: OptionStrategyTemplate) -> None:
        """Clear all positions for a strategy."""
        holding = self.strategy_holdings[strategy.strategy_name]

        self.close_underlying_position(strategy, holding)
        self.close_all_combo_positions(strategy, holding)
        self.close_all_option_positions(strategy, holding)

    def close_underlying_position(self, strategy: OptionStrategyTemplate, holding: StrategyHolding) -> None:
        """Clear underlying position for a strategy."""
        if holding.underlyingPosition.quantity == 0:
            return

        close_qty = abs(holding.underlyingPosition.quantity)
        close_direction = Direction.SHORT if holding.underlyingPosition.quantity > 0 else Direction.LONG

        strategy.underlying_order(
            direction=close_direction,
            price=0,
            volume=close_qty,
            order_type=OrderType.MARKET,
            reference=f"{APP_NAME}_{strategy.strategy_name}",
        )

    def close_all_combo_positions(self, strategy: OptionStrategyTemplate, holding: StrategyHolding) -> None:
        """Clear all combo positions for a strategy."""
        for comboPosition in holding.comboPositions.values():
            if comboPosition.quantity == 0:
                continue
            self.close_combo_position(strategy, comboPosition)

    def close_combo_position(self, strategy: OptionStrategyTemplate, comboPosition: ComboPositionData) -> None:
        """Close combo position by creating a closing combo with opposite leg directions."""

        option_data: dict[str, Any] = {}

        for leg in comboPosition.legs:
            snap = strategy.portfolio.options.get(leg.symbol)
            if snap:
                option_data[leg.symbol] = snap

        action = "SELL" if comboPosition.quantity > 0 else "BUY"
        strategy.write_log(f"Closing Combo {comboPosition.combo_type.name}: {action} {abs(comboPosition.quantity)}", INFO)

        close_direction = Direction.SHORT if comboPosition.quantity > 0 else Direction.LONG

        strategy.combo_order(
            combo_type=ComboType.CUSTOM,
            option_data=option_data,
            direction=close_direction,
            price=0,
            volume=abs(comboPosition.quantity),
            order_type=OrderType.MARKET,
            reference=f"{APP_NAME}_{strategy.strategy_name}",
        )

    def close_all_option_positions(self, strategy: OptionStrategyTemplate, holding: StrategyHolding) -> None:
        """Clear all option positions for a strategy."""
        for optionPosition in holding.optionPositions.values():
            if optionPosition.quantity == 0:
                continue
            self.close_option_position(strategy, optionPosition)

    def close_option_position(self, strategy: OptionStrategyTemplate, optionPosition: OptionPositionData) -> None:
        """Close a single option position."""
        action = "SELL" if optionPosition.quantity > 0 else "BUY"
        strategy.write_log(f"Closing Option {optionPosition.symbol}: {action} {abs(optionPosition.quantity)}", INFO)

        option_data = strategy.portfolio.options.get(optionPosition.symbol)
        if option_data:
            close_direction = Direction.SHORT if optionPosition.quantity > 0 else Direction.LONG

            strategy.option_order(
                option_data=option_data,
                direction=close_direction,
                price=0,
                volume=abs(optionPosition.quantity),
                order_type=OrderType.MARKET,
                reference=f"{APP_NAME}_{strategy.strategy_name}",
            )

    # ========================================================================
    # SERIALIZATION AND PERSISTENCE
    # ========================================================================

    def load_serialized_holding(self, strategy_name: str, data: dict) -> None:
        """Load previously saved holding into a strategy's holding container."""
        holding = self.strategy_holdings[strategy_name]

        # Load data into holding
        if "underlying" in data:
            holding.underlyingPosition = UnderlyingPositionData(**data["underlying"])
        if "options" in data:
            holding.optionPositions = {opt["symbol"]: OptionPositionData(**opt) for opt in data["options"]}
        if "combos" in data:
            holding.comboPositions = {
                c["symbol"]: ComboPositionData(
                    symbol=c["symbol"],
                    quantity=c.get("quantity", 0),
                    combo_type=ComboType[c.get("combo_type")],
                    avg_cost=c.get("avg_cost", 0.0),
                    cost_value=c.get("cost_value", 0.0),
                    realized_pnl=c.get("realized_pnl", 0.0),
                    mid_price=c.get("mid_price", 0.0),
                    delta=c.get("delta", 0.0),
                    gamma=c.get("gamma", 0.0),
                    theta=c.get("theta", 0.0),
                    vega=c.get("vega", 0.0),
                    legs=[OptionPositionData(**leg) for leg in c.get("legs", [])],
                )
                for c in data["combos"]
            }
        if "summary" in data:
            holding.summary = PortfolioSummary(**data["summary"])

    def serialize_holding(self, strategy_name: str) -> dict:
        """Build a YAML-serializable dict for a strategy's holding."""
        holding = self.strategy_holdings[strategy_name]

        return {
            "underlying": {
                "symbol": holding.underlyingPosition.symbol,
                "quantity": holding.underlyingPosition.quantity,
                "avg_cost": holding.underlyingPosition.avg_cost,
                "cost_value": holding.underlyingPosition.cost_value,
                "realized_pnl": holding.underlyingPosition.realized_pnl,
                "mid_price": holding.underlyingPosition.mid_price,
                "delta": holding.underlyingPosition.delta,
                "gamma": holding.underlyingPosition.gamma,
                "theta": holding.underlyingPosition.theta,
                "vega": holding.underlyingPosition.vega,
            },
            "options": [
                {
                    "symbol": opt.symbol,
                    "quantity": opt.quantity,
                    "avg_cost": opt.avg_cost,
                    "cost_value": opt.cost_value,
                    "realized_pnl": opt.realized_pnl,
                    "mid_price": opt.mid_price,
                    "delta": opt.delta,
                    "gamma": opt.gamma,
                    "theta": opt.theta,
                    "vega": opt.vega,
                }
                for opt in holding.optionPositions.values()
            ],
            "combos": [
                {
                    "symbol": combo.symbol,
                    "quantity": combo.quantity,
                    "combo_type": combo.combo_type.name,
                    "avg_cost": combo.avg_cost,
                    "cost_value": combo.cost_value,
                    "realized_pnl": combo.realized_pnl,
                    "mid_price": combo.mid_price,
                    "delta": combo.delta,
                    "gamma": combo.gamma,
                    "theta": combo.theta,
                    "vega": combo.vega,
                    "legs": [
                        {
                            "symbol": leg.symbol,
                            "quantity": leg.quantity,
                            "avg_cost": leg.avg_cost,
                            "cost_value": leg.cost_value,
                            "realized_pnl": leg.realized_pnl,
                            "mid_price": leg.mid_price,
                            "delta": leg.delta,
                            "gamma": leg.gamma,
                            "theta": leg.theta,
                            "vega": leg.vega,
                        }
                        for leg in combo.legs
                    ],
                }
                for combo in holding.comboPositions.values()
            ],
            "summary": {
                "total_cost": holding.summary.total_cost,
                "current_value": holding.summary.current_value,
                "unrealized_pnl": holding.summary.unrealized_pnl,
                "realized_pnl": holding.summary.realized_pnl,
                "pnl": holding.summary.pnl,
                "delta": holding.summary.delta,
                "gamma": holding.summary.gamma,
                "theta": holding.summary.theta,
                "vega": holding.summary.vega,
            },
        }

    # ========================================================================
    # UTILITY METHODS
    # ========================================================================

    @staticmethod
    def _classify_symbol(symbol: str) -> str:
        """Classify a symbol as option, underlying, or combo."""
        if symbol.endswith("-OPT"):
            return "option"
        elif symbol.endswith("-STK"):
            return "underlying"
        elif "_" in symbol:
            return "combo"
        else:
            raise ValueError(f"Unknown symbol format: {symbol}")

    @staticmethod
    def _normalize_combo_symbol(symbol: str) -> str:
        """Normalize combo symbol by removing combo type for matching."""
        parts = symbol.split("_", 2)
        if len(parts) >= 3:
            return f"{parts[0]}_{parts[2]}"
        return symbol

    @staticmethod
    def _add_totals(totals: dict, metrics: dict) -> None:
        """Add metrics to totals dictionary."""
        for k in totals.keys():
            totals[k] += metrics.get(k, 0.0)
