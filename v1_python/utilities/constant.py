"""
General constant enums used in the trading platform.
"""

from enum import Enum
from zoneinfo import ZoneInfo  # noqa: E402

from tzlocal import get_localzone_name  # noqa: E402


class Direction(Enum):
    """
    Direction of order/trade/position.
    """

    LONG = "LONG"
    SHORT = "SHORT"
    NET = "NET"


class Status(Enum):
    """
    Order status.
    """

    SUBMITTING = "SUBMITTING"
    NOTTRADED = "NOTTRADED"
    PARTTRADED = "PARTTRADED"
    ALLTRADED = "ALLTRADED"
    CANCELLED = "CANCELLED"
    REJECTED = "REJECTED"


class Product(Enum):
    """
    Product class.
    """

    EQUITY = "EQUITY"
    FUTURES = "FUTURES"
    OPTION = "OPTION"
    INDEX = "INDEX"
    FOREX = "FOREX"
    SPOT = "SPOT"
    ETF = "ETF"
    BOND = "BOND"
    WARRANT = "WARRANT"
    SPREAD = "SPREAD"
    FUND = "FUND"
    CFD = "CFD"
    SWAP = "SWAP"
    UNKNOWN = "UNKNOWN"


class OrderType(Enum):
    """
    Order type.
    """

    LIMIT = "LIMIT"
    MARKET = "MARKET"


class OptionType(Enum):
    """
    Option type.
    """

    CALL = "CALL"
    PUT = "PUT"


class ComboType(Enum):
    """
    Option combo strategy types.
    """

    CUSTOM = "custom"  # placeholder for custom combo

    # 2-leg strategies
    SPREAD = "spread"  # Vertical/Calendar spread
    STRADDLE = "straddle"  # Long/Short straddle
    STRANGLE = "strangle"  # Long/Short strangle
    DIAGONAL_SPREAD = "diagonal_spread"  # Diagonal spread
    RATIO_SPREAD = "ratio_spread"  # Ratio spread

    # Common named combos
    RISK_REVERSAL = "risk_reversal"  # Short put + long call (or inverse)

    # 3-leg strategies
    BUTTERFLY = "butterfly"  # Long/Short butterfly
    INVERSE_BUTTERFLY = "inverse_butterfly"  # Inverse butterfly

    # 4-leg strategies
    IRON_CONDOR = "ironcondor"  # Iron condor
    IRON_BUTTERFLY = "iron_butterfly"  # Iron butterfly
    CONDOR = "condor"  # Regular condor
    BOX_SPREAD = "box_spread"  # Box spread

    # Multi-leg strategies with underlying


class Exchange(Enum):
    """
    Exchange.
    """

    # Global
    SMART = "SMART"
    NYSE = "NYSE"
    NASDAQ = "NASDAQ"
    AMEX = "AMEX"
    CBOE = "CBOE"
    IBKRATS = "IBKRATS"

    # Special Function
    LOCAL = "LOCAL"  # For local generated data


# Order status mapping
STATUS_IB2VT: dict[str, Status] = {
    "ApiPending": Status.SUBMITTING,
    "PendingSubmit": Status.SUBMITTING,
    "PreSubmitted": Status.SUBMITTING,  # Changed from NOTTRADED
    "Submitted": Status.NOTTRADED,
    "ApiCancelled": Status.CANCELLED,
    "PendingCancel": Status.SUBMITTING,  # Added missing status
    "Cancelled": Status.CANCELLED,
    "Filled": Status.ALLTRADED,
    "Inactive": Status.REJECTED,
    "PartiallyFilled": Status.PARTTRADED,  # Added missing status
}

# Long/Short direction mapping
DIRECTION_VT2IB: dict[Direction, str] = {Direction.LONG: "BUY", Direction.SHORT: "SELL"}
DIRECTION_IB2VT: dict[str, Direction] = {v: k for k, v in DIRECTION_VT2IB.items()}
DIRECTION_IB2VT["BOT"] = Direction.LONG
DIRECTION_IB2VT["SLD"] = Direction.SHORT

# Order type mapping
ORDERTYPE_VT2IB: dict[OrderType, str] = {OrderType.LIMIT: "LMT", OrderType.MARKET: "MKT"}
ORDERTYPE_IB2VT: dict[str, OrderType] = {v: k for k, v in ORDERTYPE_VT2IB.items()}

# Exchange mapping
EXCHANGE_VT2IB: dict[Exchange, str] = {
    Exchange.SMART: "SMART",
    Exchange.NYSE: "NYSE",
    Exchange.NASDAQ: "NASDAQ",
    Exchange.AMEX: "AMEX",
    Exchange.CBOE: "CBOE",
    Exchange.IBKRATS: "IBKRATS",
}
EXCHANGE_IB2VT: dict[str, Exchange] = {v: k for k, v in EXCHANGE_VT2IB.items()}

# Product type mapping
PRODUCT_IB2VT: dict[str, Product] = {
    "STK": Product.EQUITY,
    "OPT": Product.OPTION,
    "FOP": Product.OPTION,
    "IND": Product.INDEX,  # Index underlyings like SPX
    "FUT": Product.FUTURES,
}

# Option type mapping
OPTION_IB2VT: dict[str, OptionType] = {"C": OptionType.CALL, "CALL": OptionType.CALL, "P": OptionType.PUT, "PUT": OptionType.PUT}


ACCOUNTFIELD_IB2VT = {
    "NetLiquidation": "balance",
    "UnrealizedPnL": "unrealized_pnl",
    "RealizedPnL": "realized_pnl",
    "AvailableFunds": "available_funds",
    "ExcessLiquidity": "excess_liquidity",
    "MaintMarginReq": "maintenance_margin",
    "BuyingPower": "buying_power",
    "CashBalance": "cash_balance",
    "GrossPositionValue": "gross_position_value",
    "EquityWithLoanValue": "equity_with_loan",
    "SMA": "sma",
}


# Other constants
LOCAL_TZ = ZoneInfo(get_localzone_name())
JOIN_SYMBOL: str = "-"
