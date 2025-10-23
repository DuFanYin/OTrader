from time import sleep
from typing import TYPE_CHECKING

from strategies.template import OptionStrategyTemplate
from utilities.constant import ComboType, Direction, OrderType

if TYPE_CHECKING:
    from engines.engine_strategy import OptionStrategyEngine
    from utilities.object import StrategyHolding
    from utilities.portfolio import OptionData


class Test(OptionStrategyTemplate):
    """
    Test strategy for checking the functionality of the strategy engine.
    It will long a straddle on the 0DTE chain.
    """

    author = "Hang Zhengyang"
    parameters = []
    variables = ["inited", "started", "error"]

    def __init__(self, strategy_engine: "OptionStrategyEngine", strategy_name: str, portfolio_name: str, setting: dict) -> None:
        super().__init__(strategy_engine, strategy_name, portfolio_name, setting)

        self.chain_symbols: list[str] = []
        self.timer_trigger = 5
        self.timer_count: int = 0

        self.legs_entry: dict[str, "OptionData"] = {}

    def on_init_logic(self) -> None:
        nearest_chain = self.portfolio.get_chain_by_expiry(5, 12)
        self.subscribe_chains(nearest_chain)

        sleep(1)

        chain = self.get_chain(nearest_chain[0])

        if chain is None:
            self.write_log("Registered chain not available")
            return

        chain.calculate_atm_price()

        target_strike = str(float(chain.atm_index))
        call_option = chain.calls.get(target_strike)
        put_option = chain.puts.get(target_strike)

        if not call_option or not put_option:
            self.write_log(f"Could not find call/put contracts for strike {target_strike}.")
            return

        self.legs_entry = {"call": call_option, "put": put_option}

    def on_stop_logic(self) -> None:
        self.write_log("Stopped")

    def on_timer_logic(self) -> None:
        self.timer_count += 1
        self.write_log(f"Timer count: {self.timer_count}")

        self._monitoring_logic()

        if self.timer_count == 1:
            self.combo_order(
                combo_type=ComboType.STRADDLE,
                option_data=self.legs_entry,
                direction=Direction.LONG,
                price=0,
                volume=1,
                order_type=OrderType.MARKET,
            )

        if self.timer_count == 5:
            self.close_all_strategy_positions()
            self.write_log("Closed all positions")

    def _monitoring_logic(self) -> None:
        holding: "StrategyHolding" = self.holding
        s = holding.summary

        pnl_pct = s.pnl / abs(s.total_cost) if s.total_cost else 0
        self.write_log(f"P&L {pnl_pct*100:.1f}% | ${s.pnl:.2f}")
