from __future__ import annotations

from typing import TYPE_CHECKING

from engines.engine_log import DEBUG, INFO
from utilities.constant import ComboType, Direction
from utilities.object import ContractData, Leg

if TYPE_CHECKING:
    from engines.engine_strategy import OptionStrategyEngine
    from utilities.portfolio import OptionData


class ComboBuilderEngine:
    """Engine responsible for building combo orders and managing combo strategies."""

    def __init__(self, option_strategy_engine: OptionStrategyEngine) -> None:
        self.option_strategy_engine = option_strategy_engine

    def write_log(self, msg: str, level: int = INFO) -> None:
        """Write log message through the strategy engine."""
        self.option_strategy_engine.write_log(msg, level)

    # ====================== LEG CREATION ======================

    def create_leg(self, option: OptionData, direction: Direction, volume: int, price: float | None = None) -> Leg:
        """Create a leg for combo orders from OptionData by resolving its contract."""
        contract: ContractData | None = self.option_strategy_engine.get_contract(option.symbol)
        if not contract:
            raise ValueError(f"Contract not found for option: {option.symbol}")

        leg = Leg(
            con_id=contract.con_id or 0,
            symbol=contract.symbol,
            exchange=contract.exchange,
            direction=direction,
            ratio=volume,
            price=price,
            gateway_name="IB",
            trading_class=contract.trading_class,
        )

        return leg

    def combo_builder(
        self, option_data: dict[str, OptionData], combo_type: ComboType, direction: Direction, volume: int
    ) -> tuple[list[Leg], str]:
        """Create combo from option data based on combo type.

        Args:
            option_data: Dictionary mapping leg names to OptionData objects
                - For STRADDLE: {"call": OptionData, "put": OptionData}
                - For STRANGLE: {"call": OptionData, "put": OptionData}
                - For SPREAD: {"long_leg": OptionData, "short_leg": OptionData}
                - For DIAGONAL_SPREAD: {"long_leg": OptionData, "short_leg": OptionData}
                - For RATIO_SPREAD: {"long_leg": OptionData, "short_leg": OptionData, "ratio": int}
                - For BUTTERFLY: {"body": OptionData, "wing1": OptionData, "wing2": OptionData}
                - For INVERSE_BUTTERFLY: {"body": OptionData, "wing1": OptionData, "wing2": OptionData}
                - For IRON_BUTTERFLY: {"put_wing": OptionData, "body": OptionData, "call_wing": OptionData}
                - For IRON_CONDOR: {"put_lower": OptionData, "put_upper": OptionData, "call_lower": OptionData, "call_upper": OptionData}
                - For CONDOR: {"long_put": OptionData, "short_put": OptionData, "short_call": OptionData, "long_call": OptionData}
                - For BOX_SPREAD: {"long_call": OptionData, "short_call": OptionData, "short_put": OptionData, "long_put": OptionData}
                - For RISK_REVERSAL: {"long_leg": OptionData, "short_leg": OptionData}
                - For CUSTOM: {"leg1": OptionData, "leg2": OptionData, ...}
        """

        if combo_type == ComboType.STRADDLE:
            return self.straddle(option_data, direction, volume)

        elif combo_type == ComboType.STRANGLE:
            return self.strangle(option_data, direction, volume)

        elif combo_type == ComboType.IRON_CONDOR:
            return self.iron_condor(option_data, direction, volume)

        elif combo_type == ComboType.RISK_REVERSAL:
            return self.risk_reversal(option_data, direction, volume)

        elif combo_type == ComboType.SPREAD:
            return self.spread(option_data, direction, volume)

        elif combo_type == ComboType.DIAGONAL_SPREAD:
            return self.diagonal_spread(option_data, direction, volume)

        elif combo_type == ComboType.RATIO_SPREAD:
            return self.ratio_spread(option_data, direction, volume)

        elif combo_type == ComboType.BUTTERFLY:
            return self.butterfly(option_data, direction, volume)

        elif combo_type == ComboType.INVERSE_BUTTERFLY:
            return self.inverse_butterfly(option_data, direction, volume)

        elif combo_type == ComboType.IRON_BUTTERFLY:
            return self.iron_butterfly(option_data, direction, volume)

        elif combo_type == ComboType.CONDOR:
            return self.condor(option_data, direction, volume)

        elif combo_type == ComboType.BOX_SPREAD:
            return self.box_spread(option_data, direction, volume)

        elif combo_type == ComboType.CUSTOM:
            return self.custom(option_data, direction, volume)

        else:
            raise ValueError(f"Unsupported combo type: {combo_type}")

    # ====================== COMBO BUILDERS ======================

    def straddle(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build straddle combo: ATM call + ATM put."""

        legs = [self.create_leg(option_data["call"], direction, volume), self.create_leg(option_data["put"], direction, volume)]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def strangle(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build strangle combo: OTM call + OTM put."""

        legs = [self.create_leg(option_data["call"], direction, volume), self.create_leg(option_data["put"], direction, volume)]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def iron_condor(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build iron condor combo: Long put + Short put + Short call + Long call."""
        sign = 1 if direction == Direction.SHORT else -1

        legs = [
            self.create_leg(option_data["put_lower"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["put_upper"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
            self.create_leg(option_data["call_lower"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
            self.create_leg(option_data["call_upper"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
        ]

        sig = self.generate_combo_signature(legs)
        return legs, sig

    def risk_reversal(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build risk reversal combo: Long call + Short put."""

        sign = 1 if direction == Direction.SHORT else -1

        legs = [
            self.create_leg(option_data["long_leg"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["short_leg"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
        ]
        sig = self.generate_combo_signature(legs)

        return legs, sig

    def custom(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build custom combo from option data Used to close positions"""
        legs: list[Leg] = []
        for option in option_data.values():
            # For closing positions, use the specified direction for all legs
            # This is correct - when closing a LONG position, we SHORT all legs
            leg = self.create_leg(option, direction, volume)
            legs.append(leg)
            # Debug: Log leg creation for closing positions
            self.write_log(f"Custom Combo Leg: {leg.symbol} | Direction: {leg.direction} | Volume: {leg.ratio}", DEBUG)
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def spread(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build vertical/calendar spread combo: Long leg + Short leg."""
        sign = 1 if direction == Direction.LONG else -1

        legs = [
            self.create_leg(option_data["long_leg"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["short_leg"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
        ]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def diagonal_spread(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build diagonal spread combo: Long leg + Short leg (different strikes and expirations)."""
        sign = 1 if direction == Direction.LONG else -1

        legs = [
            self.create_leg(option_data["long_leg"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["short_leg"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
        ]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def ratio_spread(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build ratio spread combo: Long leg + Multiple short legs."""
        sign = 1 if direction == Direction.LONG else -1
        ratio = option_data.get("ratio", 2)  # Default 1:2 ratio

        legs = [
            self.create_leg(option_data["long_leg"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["short_leg"], Direction.SHORT if sign > 0 else Direction.LONG, volume * ratio),
        ]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def butterfly(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build butterfly combo: Long body + Short wings."""
        sign = 1 if direction == Direction.LONG else -1

        legs = [
            self.create_leg(option_data["body"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["wing1"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
            self.create_leg(option_data["wing2"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
        ]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def inverse_butterfly(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build inverse butterfly combo: Short body + Long wings."""
        sign = 1 if direction == Direction.LONG else -1

        legs = [
            self.create_leg(option_data["body"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
            self.create_leg(option_data["wing1"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["wing2"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
        ]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def iron_butterfly(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build iron butterfly combo: Long put wing + Short body + Long call wing."""
        sign = 1 if direction == Direction.LONG else -1

        legs = [
            self.create_leg(option_data["put_wing"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["body"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
            self.create_leg(option_data["call_wing"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
        ]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def condor(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build condor combo: Long put + Short put + Short call + Long call."""
        sign = 1 if direction == Direction.LONG else -1

        legs = [
            self.create_leg(option_data["long_put"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["short_put"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
            self.create_leg(option_data["short_call"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
            self.create_leg(option_data["long_call"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
        ]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    def box_spread(self, option_data: dict[str, OptionData], direction: Direction, volume: int) -> tuple[list[Leg], str]:
        """Build box spread combo: Long call + Short call + Short put + Long put."""
        sign = 1 if direction == Direction.LONG else -1

        legs = [
            self.create_leg(option_data["long_call"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
            self.create_leg(option_data["short_call"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
            self.create_leg(option_data["short_put"], Direction.SHORT if sign > 0 else Direction.LONG, volume),
            self.create_leg(option_data["long_put"], Direction.LONG if sign > 0 else Direction.SHORT, volume),
        ]
        sig = self.generate_combo_signature(legs)
        return legs, sig

    @staticmethod
    def generate_combo_signature(legs: list[Leg]) -> str:
        """Build unique signature string from multiple legs."""
        leg_signatures = []
        for leg in legs:
            if not leg.symbol:
                continue
            parts = leg.symbol.split("-")
            expiry, opt_type, strike = parts[1:4]
            leg_signatures.append(f"{expiry}{opt_type}{strike}")
        return "-".join(sorted(leg_signatures))
