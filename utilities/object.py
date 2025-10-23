from __future__ import annotations

import datetime as dt
from dataclasses import dataclass, field
from logging import INFO

from .constant import ComboType, Direction, Exchange, OptionType, OrderType, Product, Status

"""
Basic data structure used for general trading function in the trading platform.
"""

ACTIVE_STATUSES = {Status.SUBMITTING, Status.NOTTRADED, Status.PARTTRADED}


@dataclass
class BaseData:
    """
    Any data object needs a gateway_name as source
    and should inherit base data.
    """

    gateway_name: str
    extra: dict | None = field(default=None, init=False)


# ------------------------------
# Market Data Objects
# ------------------------------


@dataclass
class TickData(BaseData):
    """
    Tick data for underlying market data, containing:
        * last trade price
        * bid/ask prices
        * volume and turnover
        * open interest
    Used primarily for underlying asset price updates.
    For option market data, see OptionMarketData class.
    """

    symbol: str
    exchange: Exchange
    datetime: dt.datetime

    name: str = ""
    volume: float = 0
    turnover: float = 0
    open_interest: float = 0
    last_price: float = 0
    last_volume: float = 0
    bid_price_1: float = 0
    ask_price_1: float = 0
    localtime: dt.datetime | None = None


@dataclass
class OptionMarketData(BaseData):
    """
    Market data for a single option contract, including price and Greeks.
    """

    symbol: str
    exchange: Exchange
    datetime: dt.datetime

    bid_price: float = 0.0
    ask_price: float = 0.0
    last_price: float = 0.0
    volume: float = 0.0
    open_interest: float = 0.0

    delta: float = 0.0
    gamma: float = 0.0
    theta: float = 0.0
    vega: float = 0.0
    mid_iv: float = 0.0


@dataclass
class ChainMarketData(BaseData):
    """
    Market data for an entire option chain, including underlying and all options.
    """

    chain_symbol: str
    datetime: dt.datetime

    underlying_symbol: str
    underlying_bid: float = 0.0
    underlying_ask: float = 0.0
    underlying_last: float = 0.0

    options: dict[str, OptionMarketData] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if not self.underlying_last and (self.underlying_bid or self.underlying_ask):
            self.underlying_last = (
                (self.underlying_bid + self.underlying_ask) / 2
                if self.underlying_bid and self.underlying_ask
                else self.underlying_bid or self.underlying_ask
            )

    def add_option(self, option_data: OptionMarketData) -> None:
        """Add or update option market data in the chain keyed by symbol"""
        self.options[option_data.symbol] = option_data


# ------------------------------
# Contract and Position Objects
# ------------------------------


@dataclass
class ContractData(BaseData):
    """
    Contract data contains basic information about each contract traded.
    """

    symbol: str
    exchange: Exchange
    name: str
    product: Product
    size: float
    pricetick: float

    min_volume: float = 1
    max_volume: float | None = None
    stop_supported: bool = False
    net_position: bool = False
    history_data: bool = False
    con_id: int | None = None
    trading_class: str | None = None

    option_strike: float | None = None
    option_underlying: str | None = None
    option_type: OptionType | None = None
    option_listed: dt.datetime | None = None
    option_expiry: dt.datetime | None = None
    option_portfolio: str | None = None
    option_index: str | None = None


@dataclass
class PositionData(BaseData):
    """
    Position data is used for tracking each individual position holding.
    """

    symbol: str
    exchange: Exchange
    direction: Direction

    volume: float = 0
    frozen: float = 0
    price: float = 0
    pnl: float = 0
    yd_volume: float = 0

    def __post_init__(self) -> None:
        self.positionid = f"{self.gateway_name}.{self.symbol}.{self.direction}"


# ------------------------------
# Order Related Objects
# ------------------------------


@dataclass
class Leg(BaseData):
    """
    ComboLeg definition: One leg corresponds to one specific contract.
    """

    con_id: int
    exchange: Exchange
    ratio: int
    direction: Direction
    price: float | None = None
    symbol: str | None = None
    trading_class: str | None = None


@dataclass
class TradeData(BaseData):
    """
    Trade data contains information of a fill of an order. One order
    can have several trade fills.
    """

    symbol: str
    exchange: Exchange
    orderid: str
    tradeid: str
    direction: Direction | None = None

    price: float = 0
    volume: float = 0
    datetime: dt.datetime | None = None


# ------------------------------
# Account and System Objects
# ------------------------------


@dataclass
class AccountData(BaseData):
    """
    Account data contains information about balance, frozen, available,
    margin requirements, and profit/loss.
    """

    accountid: str

    balance: float = 0
    frozen: float = 0
    margin: float = 0
    position_profit: float = 0

    def __post_init__(self) -> None:
        self.available: float = self.balance - self.frozen
        self.accountid: str = f"{self.gateway_name}.{self.accountid}"


@dataclass
class LogData(BaseData):
    """
    Log data is used for recording log messages on GUI or in log files.
    """

    msg: str
    level: int = INFO

    def __post_init__(self) -> None:
        now = dt.datetime.now().replace(microsecond=0)
        self.time: str = now.strftime("%m-%d %H:%M:%S")


# ------------------------------
# Request Objects
# ------------------------------


@dataclass
class OrderData(BaseData):
    """
    Order data contains information for tracking latest status of a specific order.
    """

    symbol: str
    exchange: Exchange
    orderid: str
    trading_class: str | None = None

    type: OrderType = OrderType.LIMIT
    direction: Direction | None = None
    price: float | None = 0
    volume: float = 0
    traded: float = 0
    status: Status = Status.SUBMITTING
    datetime: dt.datetime | None = None
    reference: str = ""

    is_combo: bool = False
    legs: list[Leg] | None = None
    combo_type: ComboType | None = None

    def is_active(self) -> bool:
        return self.status in ACTIVE_STATUSES

    def create_cancel_request(self) -> CancelRequest:
        req: CancelRequest = CancelRequest(orderid=self.orderid, symbol=self.symbol, exchange=self.exchange)
        return req


@dataclass
class OrderRequest:
    """
    Request sending to specific gateway for creating a new order.
    """

    symbol: str
    exchange: Exchange
    direction: Direction
    type: OrderType
    volume: float
    price: float = 0
    reference: str = ""
    trading_class: str | None = None

    is_combo: bool = False
    legs: list[Leg] | None = None
    combo_type: ComboType | None = None

    def create_order_data(self, orderid: str, gateway_name: str) -> OrderData:
        order = OrderData(
            symbol=self.symbol,
            exchange=self.exchange,
            orderid=orderid,
            trading_class=self.trading_class,
            type=self.type,
            direction=self.direction,
            combo_type=self.combo_type,
            price=self.price,
            volume=self.volume,
            reference=self.reference,
            gateway_name=gateway_name,
            is_combo=self.is_combo,
            legs=self.legs if self.legs else None,
        )
        return order


@dataclass
class CancelRequest:
    """
    Request for canceling an existing order.
    """

    orderid: str
    symbol: str
    exchange: Exchange
    is_combo: bool = False
    legs: list[Leg] | None = None


# ------------------------------
# Position Holding Related Objects
# ------------------------------


@dataclass
class BasePosition:
    symbol: str
    quantity: int = 0
    avg_cost: float = 0.0
    cost_value: float = 0.0
    realized_pnl: float = 0.0
    mid_price: float = 0.0
    delta: float = 0.0
    gamma: float = 0.0
    theta: float = 0.0
    vega: float = 0.0
    multiplier: float = 1.0

    @property
    def current_value(self) -> float:
        return self.quantity * self.mid_price * self.multiplier

    def clear_fields(self) -> None:
        """Clear all fields except realized_pnl when quantity is zero."""
        if self.quantity == 0:
            self.avg_cost = 0.0
            self.cost_value = 0.0
            self.mid_price = 0.0
            self.delta = 0.0
            self.gamma = 0.0
            self.theta = 0.0
            self.vega = 0.0


@dataclass
class OptionPositionData(BasePosition):
    multiplier: float = 100.0


@dataclass
class UnderlyingPositionData(BasePosition):
    symbol: str = "Underlying"
    delta: float = 1.0


@dataclass
class ComboPositionData(BasePosition):
    combo_type: ComboType = ComboType.CUSTOM
    legs: list[OptionPositionData] = field(default_factory=list)
    multiplier: float = 100.0

    def clear_fields(self) -> None:
        """Clear all fields except realized_pnl when quantity is zero, including legs."""
        super().clear_fields()
        # Also clear individual leg fields
        for leg in self.legs:
            leg.clear_fields()


@dataclass
class PortfolioSummary:
    total_cost: float = 0.0
    current_value: float = 0.0
    unrealized_pnl: float = 0.0
    realized_pnl: float = 0.0
    pnl: float = 0.0
    delta: float = 0.0
    gamma: float = 0.0
    theta: float = 0.0
    vega: float = 0.0


@dataclass
class StrategyHolding:
    """Pure data container for strategy holdings - no logic."""

    underlyingPosition: UnderlyingPositionData = field(default_factory=UnderlyingPositionData)
    optionPositions: dict[str, OptionPositionData] = field(default_factory=dict)
    comboPositions: dict[str, ComboPositionData] = field(default_factory=dict)
    summary: PortfolioSummary = field(default_factory=PortfolioSummary)
