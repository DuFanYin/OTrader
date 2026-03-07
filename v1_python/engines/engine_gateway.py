from copy import copy
from datetime import datetime
from decimal import Decimal
from threading import Thread
from typing import TYPE_CHECKING, Any
from zoneinfo import ZoneInfo

from ibapi.client import EClient
from ibapi.common import OrderId, TickerId
from ibapi.contract import ComboLeg, Contract, ContractDetails
from ibapi.execution import Execution
from ibapi.order import Order
from ibapi.order_state import OrderState
from ibapi.wrapper import EWrapper

from .engine_event import Event, EventEngine

if TYPE_CHECKING:
    from .engine_main import MainEngine

from engines.engine_log import DEBUG, ERROR, INFO, WARNING
from utilities.constant import (
    ACCOUNTFIELD_IB2VT,
    DIRECTION_IB2VT,
    DIRECTION_VT2IB,
    EXCHANGE_IB2VT,
    EXCHANGE_VT2IB,
    JOIN_SYMBOL,
    LOCAL_TZ,
    OPTION_IB2VT,
    ORDERTYPE_IB2VT,
    ORDERTYPE_VT2IB,
    PRODUCT_IB2VT,
    STATUS_IB2VT,
    Direction,
    Exchange,
    OrderType,
    Product,
    Status,
)
from utilities.event import EVENT_ACCOUNT, EVENT_CONTRACT, EVENT_LOG, EVENT_ORDER, EVENT_POSITION, EVENT_TIMER, EVENT_TRADE
from utilities.object import AccountData, CancelRequest, ContractData, Leg, LogData, OrderData, OrderRequest, PositionData, TradeData

"""
### IB Contract Symbol Format
1. Option Contract:

   {underlying}-{expiry}-{type}-{strike}-{multiplier}-USD-OPT
   Example: `SPY-20240621-C-400-100-USD-OPT`

2. Underlying Stock:

   {symbol}-USD-STK
   Example: `SPY-USD-STK`

ConId is also supported for symbol.
"""


APP_NAME = "IBGateway"


class IbGateway:
    """Interactive Brokers gateway for VeighNa trading platform."""

    default_setting: dict = {"TWS Address": "127.0.0.1", "TWS Port": 4002, "Client ID": 1, "Trading Account": ""}
    exchanges: list[Exchange] = list(EXCHANGE_VT2IB.keys())

    def __init__(self, main_engine: "MainEngine", event_engine: EventEngine) -> None:
        """Initialize IB gateway."""
        self.main_engine = main_engine
        self.event_engine = event_engine
        self.gateway_name = APP_NAME
        self.api = IbApi(self)
        self.count = 0

    def on_event(self, event_type: str, data: object = None) -> None:
        """Push event to event engine."""
        event = Event(event_type, data)
        self.event_engine.put(event)

    def on_trade(self, trade: TradeData) -> None:
        """Push trade event."""
        self.on_event(EVENT_TRADE, trade)

    def on_order(self, order: OrderData) -> None:
        """Push order event."""
        self.on_event(EVENT_ORDER, order)

    def on_position(self, position: PositionData) -> None:
        """Push position event."""
        self.on_event(EVENT_POSITION, position)

    def on_account(self, account: AccountData) -> None:
        """Push account event."""
        self.on_event(EVENT_ACCOUNT, account)

    def on_log(self, log: LogData) -> None:
        """Push log event."""
        self.on_event(EVENT_LOG, log)

    def on_contract(self, contract: ContractData) -> None:
        """Push contract event."""
        self.on_event(EVENT_CONTRACT, contract)

    def connect(self) -> None:
        """Connect to IB trading interface."""
        self.api.connect(
            self.default_setting["TWS Address"],
            self.default_setting["TWS Port"],
            self.default_setting["Client ID"],
            self.default_setting["Trading Account"],
        )
        self.event_engine.register(EVENT_TIMER, self.process_timer_event)

    def disconnect(self) -> None:
        """Disconnect from IB interface."""
        self.api.close()

    def send_order(self, req: OrderRequest) -> str:
        """Send order request."""
        return self.api.send_order(req)

    def cancel_order(self, req: CancelRequest) -> None:
        """Cancel order request."""
        self.api.cancel_order(req)

    def query_account(self) -> None:
        """Query account information."""
        self.api.query_account()

    def query_position(self) -> None:
        """Query position information."""
        self.api.query_position()

    def query_portfolio(self, underlying: str) -> None:
        """Query underlying portfolio."""
        self.api.query_portfolio(underlying)

    def process_timer_event(self, event: Event) -> None:
        """Process timer event for connection check."""
        self.count += 1
        if self.count < 10:
            return
        self.count = 0
        self.api.check_connection()

    def write_log(self, msg: str, level: int = INFO) -> None:
        """Write log message."""
        log = LogData(msg=msg, level=level, gateway_name=self.gateway_name)
        self.on_log(log)


class IbApi(EWrapper, EClient):
    """Interactive Brokers API interface."""

    def __init__(self, gateway: IbGateway) -> None:
        """Initialize IB API with gateway reference and data structures."""
        EWrapper.__init__(self)
        EClient.__init__(self, self)

        self.gateway = gateway
        self.gateway_name = gateway.gateway_name

        # Connection state
        self.status = False
        self.orderid = 0
        self.reqid = 0
        self.clientid = 0
        self.account = ""

        # Order management
        self.orders: dict[str, OrderData] = {}
        self._last_order_status: dict[str, tuple[Status, float]] = {}
        self._pending_orders: set[str] = set()
        self._completed_orders: set[str] = set()

        # Contract management
        self.contracts: dict[str, ContractData] = {}
        self.reqid_underlying_map: dict[int, Contract] = {}

        # Account data management
        self.accounts: dict[str, AccountData] = {}
        self._reqid_counter: int = 9000

    # ====================
    # Connection Management
    # ====================

    def connect(self, host: str, port: int, clientid: int, account: str) -> None:
        """Connect to TWS/IB Gateway."""
        if self.status:
            return

        self.clientid = clientid
        self.account = account

        try:
            super().connect(host, port, clientid)
            self.thread = Thread(target=self.run)
            self.thread.start()
        except Exception as e:
            self.status = False
            self.gateway.write_log(f"IB connection failed: {e}", ERROR)

    def close(self) -> None:
        """Disconnect from TWS/IB Gateway."""
        if not self.status:
            return

        self.clear_account_data()
        self.status = False
        self.disconnect()

    def check_connection(self) -> None:
        """Check and reconnect if connection is lost."""
        if self.isConnected():
            return

        if self.status:
            self.close()

        self.connect(self.host, self.port, self.clientid, self.account)

    def clear_account_data(self) -> None:
        """Clear all account data to prevent stale data on reconnect."""
        self.accounts.clear()

    def connectAck(self) -> None:
        """Handle connection acknowledgment."""
        if not self.status:
            self.status = True
            self.gateway.write_log("IB TWS connection successful", INFO)

    def connectionClosed(self) -> None:
        """Handle connection closed event."""
        self.clear_account_data()
        self.status = False
        self.gateway.write_log("IB TWS connection disconnected", WARNING)

    def nextValidId(self, orderId: int) -> None:
        """Handle next valid order ID."""
        super().nextValidId(orderId)
        if not self.orderid:
            self.orderid = orderId

    def managedAccounts(self, accountsList: str) -> None:
        """Handle managed accounts list."""
        super().managedAccounts(accountsList)

        if not self.account:
            for account_code in accountsList.split(","):
                if account_code:
                    self.account = account_code
                    self.gateway.write_log(f"Using account: {self.account}", INFO)
                    break

    # ====================
    # Error Handling
    # ====================

    def error(self, reqId: TickerId, errorCode: int, errorString: str, advancedOrderRejectJson: str = "") -> None:
        """Handle and log IB API errors."""
        harmless_codes = {202, 2104, 2106, 2158}  # 202 = Order Canceled (success message)

        if errorCode in harmless_codes:
            return

        self.gateway.write_log(f"Error [{errorCode}]: {errorString} (reqId={reqId})", ERROR)

    # ====================
    # Account & Position Management
    # ====================

    def query_account(self) -> None:
        """Query account information in snapshot mode."""
        if not self.status:
            return

        self.accounts.clear()
        self._reqid_counter += 1

        try:
            self.reqAccountSummary(self._reqid_counter, "All", "NetLiquidation,AvailableFunds,MaintMarginReq,UnrealizedPnL")
        except Exception as e:
            self.gateway.write_log(f"Failed to query account information: {e}", ERROR)

    def query_position(self) -> None:
        """Query position information."""
        if not self.status:
            return

        try:
            self.reqPositions()
        except Exception as e:
            self.gateway.write_log(f"Failed to query position information: {e}", ERROR)

    def updateAccountValue(self, key: str, val: str, currency: str, accountName: str) -> None:
        """Process account value updates (legacy support)."""
        super().updateAccountValue(key, val, currency, accountName)
        # Streaming functionality removed - this method is kept for compatibility

    def updateAccountTime(self, timeStamp: str) -> None:
        """Process account time updates (legacy support)."""
        super().updateAccountTime(timeStamp)
        # Streaming functionality removed - this method is kept for compatibility

    def accountSummary(self, reqId: int, account: str, tag: str, value: str, currency: str) -> None:
        """Process account summary callback for snapshot mode."""
        super().accountSummary(reqId, account, tag, value, currency)

        if tag not in ACCOUNTFIELD_IB2VT:
            return

        accountid = f"{account}.{currency}"

        account_data = self.accounts.get(accountid)
        if not account_data:
            account_data = AccountData(accountid=accountid, gateway_name=self.gateway_name)
            self.accounts[accountid] = account_data

        field_name = ACCOUNTFIELD_IB2VT[tag]
        setattr(account_data, field_name, float(value))

    def accountSummaryEnd(self, reqId: int) -> None:
        """Process account summary end callback for snapshot completion."""
        super().accountSummaryEnd(reqId)

        # Send all accounts
        for account in self.accounts.values():
            self.gateway.on_account(copy(account))

        # Clear data
        self.accounts.clear()

    def updatePortfolio(
        self,
        contract: Contract,
        position: Decimal,
        marketPrice: float,
        marketValue: float,
        averageCost: float,
        unrealizedPNL: float,
        realizedPNL: float,
        accountName: str,
    ) -> None:
        """Position update callback"""
        super().updatePortfolio(contract, position, marketPrice, marketValue, averageCost, unrealizedPNL, realizedPNL, accountName)

        if contract.exchange:
            exch: Exchange | None = EXCHANGE_IB2VT.get(contract.exchange, None)
        elif contract.primaryExchange:
            exch = EXCHANGE_IB2VT.get(contract.primaryExchange, None)
        else:
            exch = Exchange.SMART  # Use smart routing for default

        if not exch:
            msg: str = (
                f"Unsupported exchange position: {generate_formatted_symbol(contract)} {contract.exchange} {contract.primaryExchange}"
            )
            self.gateway.write_log(msg, WARNING)
            return

        try:
            ib_size: int = int(contract.multiplier)
        except ValueError:
            ib_size = 1
        price = averageCost / ib_size

        pos: PositionData = PositionData(
            symbol=generate_formatted_symbol(contract),
            exchange=exch,
            direction=Direction.NET,
            volume=float(position),
            price=price,
            pnl=unrealizedPNL,
            gateway_name=self.gateway_name,
        )
        self.gateway.on_position(pos)

    # Callbacks for reqPositions()
    def position(self, account: str, contract: Contract, position: Decimal, avgCost: float) -> None:
        """Handle position snapshot line from reqPositions()."""
        super().position(account, contract, position, avgCost)

        if contract.exchange:
            exch: Exchange | None = EXCHANGE_IB2VT.get(contract.exchange, None)
        elif contract.primaryExchange:
            exch = EXCHANGE_IB2VT.get(contract.primaryExchange, None)
        else:
            exch = Exchange.SMART

        if not exch:
            msg: str = (
                f"Unsupported exchange position: {generate_formatted_symbol(contract)} {contract.exchange} {contract.primaryExchange}"
            )
            self.gateway.write_log(msg, WARNING)
            return

        try:
            ib_size: int = int(contract.multiplier)
        except ValueError:
            ib_size = 1

        price = (avgCost / ib_size) if avgCost else 0.0

        pos: PositionData = PositionData(
            symbol=generate_formatted_symbol(contract),
            exchange=exch,
            direction=Direction.NET,
            volume=float(position),
            price=price,
            pnl=0.0,
            gateway_name=self.gateway_name,
        )
        self.gateway.on_position(pos)

    def positionEnd(self) -> None:
        """End of positions snapshot."""
        super().positionEnd()
        self.gateway.write_log("Position snapshot completed", INFO)

    # ====================
    # Order Management
    # ====================

    def send_order(self, req: OrderRequest) -> str:
        """Send order to IB - supports LIMIT and MARKET only."""

        if not self.status:
            return ""

        if req.type not in (OrderType.LIMIT, OrderType.MARKET):
            self.gateway.write_log(f"Unsupported order type: {req.type}", ERROR)
            return ""

        self.orderid += 1
        orderid = str(self.orderid)

        # Generate IB contract
        if req.is_combo:
            if not req.legs:
                self.gateway.write_log("Combo order requires legs", ERROR)
                return ""
            ib_contract = generate_ib_combo_contract(req.legs, req.trading_class)
        else:
            ib_contract = generate_ib_single_contract(req.symbol, req.trading_class)

        if not ib_contract:
            self.gateway.write_log("Contract generation failed", ERROR)
            return ""

        # Build IB order
        ib_order = Order()
        ib_order.orderId = self.orderid
        ib_order.clientId = self.clientid

        # For combo orders, always use BUY as the overall action
        # Individual leg directions are controlled by ComboLeg.action
        if req.is_combo:
            ib_order.action = "BUY"  # Always BUY for combo orders
        else:
            ib_order.action = DIRECTION_VT2IB[req.direction]

        ib_order.orderType = ORDERTYPE_VT2IB[req.type]
        ib_order.totalQuantity = Decimal(req.volume)
        ib_order.account = self.account
        ib_order.eTradeOnly = False
        ib_order.firmQuoteOnly = False

        # Pricing logic
        if req.type == OrderType.LIMIT:
            ib_order.lmtPrice = float(req.price)

        # Create and cache local order record
        order = req.create_order_data(orderid, self.gateway_name)
        self.orders[orderid] = order
        self._last_order_status[orderid] = (Status.SUBMITTING, 0)
        self._pending_orders.add(orderid)
        self.gateway.on_order(copy(order))

        # Send to IB
        try:
            self.placeOrder(self.orderid, ib_contract, ib_order)
        except Exception as e:
            self.gateway.write_log(f"Order failed: {e}", ERROR)
            return ""

        self.reqIds(1)
        return order.orderid

    def cancel_order(self, req: CancelRequest) -> None:
        # Cancel existing order
        if not self.status:
            return

        order = Order()
        order.orderId = int(req.orderid)
        now = datetime.now(LOCAL_TZ)
        order.manualOrderTime = now.strftime("%Y%m%d-%H:%M:%S")
        order.manualOrderCancelTime = now.strftime("%Y%m%d-%H:%M:%S")

        self.cancelOrder(int(req.orderid), order)

    # ====================
    # Order Callbacks
    # ====================

    def orderStatus(
        self,
        orderId: OrderId,
        status: str,
        filled: Decimal,
        remaining: Decimal,
        avgFillPrice: float,
        permId: int,
        parentId: int,
        lastFillPrice: float,
        clientId: int,
        whyHeld: str,
        mktCapPrice: float,
    ) -> None:
        # Handle order status updates from IB
        super().orderStatus(
            orderId, status, filled, remaining, avgFillPrice, permId, parentId, lastFillPrice, clientId, whyHeld, mktCapPrice
        )

        orderid = str(orderId)
        order: OrderData | None = self.orders.get(orderid)
        if not order:
            return

        new_filled = float(filled)
        new_status = STATUS_IB2VT.get(status, Status.SUBMITTING)

        last_status = self._last_order_status.get(orderid, None)
        current_status = (new_status, new_filled)

        if current_status != last_status:
            order.traded = new_filled
            order.status = new_status
            self._last_order_status[orderid] = current_status
            self.gateway.on_order(copy(order))

            if new_status in [Status.ALLTRADED, Status.CANCELLED, Status.REJECTED]:
                self._last_order_status.pop(orderid, None)
                self.orders.pop(orderid, None)
                self._completed_orders.add(orderid)

    def openOrder(self, orderId: OrderId, ib_contract: Contract, ib_order: Order, orderState: OrderState) -> None:
        # Handle new order notifications from IB
        orderid = str(orderId)

        if orderid in self._pending_orders:
            self._pending_orders.remove(orderid)
            return

        if orderid in self._completed_orders or orderid in self.orders:
            return

        order: OrderData | None = self.orders.get(orderid)
        if not order:
            symbol = generate_formatted_symbol(ib_contract)

            order = OrderData(
                symbol=symbol,
                exchange=Exchange.SMART,
                type=ORDERTYPE_IB2VT[ib_order.orderType],
                orderid=orderid,
                direction=DIRECTION_IB2VT.get(ib_order.action, Direction.LONG),  # Default to LONG if unknown
                volume=ib_order.totalQuantity,
                price=ib_order.lmtPrice if ib_order.orderType == "LMT" else 0,
                datetime=datetime.now(LOCAL_TZ),
                gateway_name=self.gateway_name,
            )

        self.orders[orderid] = order
        self._last_order_status[orderid] = (Status.SUBMITTING, 0)
        self.gateway.on_order(copy(order))

    def execDetails(self, reqId: int, contract: Contract, execution: Execution) -> None:
        # Handle trade execution details from IB
        super().execDetails(reqId, contract, execution)

        time_str: str = execution.time
        time_split: list = time_str.split(" ")
        words_count: int = 3

        if len(time_split) == words_count:
            timezone = time_split[-1]
            time_str = time_str.replace(f" {timezone}", "")
            tz = ZoneInfo(timezone)
        elif len(time_split) == (words_count - 1):
            tz = LOCAL_TZ
        else:
            self.gateway.write_log(f"Received unsupported time format: {time_str}", WARNING)
            return

        dt: datetime = datetime.strptime(time_str, "%Y%m%d %H:%M:%S")
        dt = dt.replace(tzinfo=tz)

        if tz != LOCAL_TZ:
            dt = dt.astimezone(LOCAL_TZ)

        orderid: str = str(execution.orderId)
        order: OrderData | None = self.orders.get(orderid)

        if order:
            symbol: str = order.symbol
            exchange: Exchange = Exchange.SMART
        else:
            symbol = generate_formatted_symbol(contract)
            exchange = Exchange.SMART

        # For combo trades, use the original order direction instead of IB's execution.side
        # IB always reports combo executions as "BOT" (Buy) regardless of actual direction
        direction = DIRECTION_IB2VT.get(execution.side, Direction.LONG)  # Default to LONG if unknown
        if order and order.is_combo and order.direction is not None:
            direction = order.direction

        trade: TradeData = TradeData(
            symbol=symbol,
            exchange=exchange,
            orderid=orderid,
            tradeid=str(execution.execId),
            direction=direction,
            price=execution.price,
            volume=float(execution.shares),
            datetime=dt,
            gateway_name=self.gateway_name,
        )

        self.gateway.on_trade(trade)

    # ====================
    # Contract Management
    # ====================

    def convert_contract_detail(self, reqId: int, detail: ContractDetails) -> None:
        # Convert IB contract details to internal format
        ib_contract = detail.contract
        if not ib_contract.multiplier:
            ib_contract.multiplier = 1

        symbol = generate_formatted_symbol(ib_contract)

        product: Product | None = PRODUCT_IB2VT.get(ib_contract.secType)
        if not product:
            return

        exchange = EXCHANGE_IB2VT.get(ib_contract.exchange)
        if not exchange:
            return

        contract_data = ContractData(
            symbol=symbol,
            exchange=exchange,
            name=detail.longName,
            product=PRODUCT_IB2VT[ib_contract.secType],
            size=float(ib_contract.multiplier),
            pricetick=detail.minTick,
            min_volume=detail.minSize,
            net_position=True,
            history_data=True,
            stop_supported=True,
            gateway_name=self.gateway_name,
            con_id=ib_contract.conId,
            trading_class=ib_contract.tradingClass,
        )

        if contract_data.product == Product.OPTION:
            underlying_symbol = str(detail.underConId)
            contract_data.option_portfolio = underlying_symbol + "_O"
            contract_data.option_type = OPTION_IB2VT.get(ib_contract.right, None)
            contract_data.option_strike = ib_contract.strike
            contract_data.option_index = str(ib_contract.strike)
            contract_data.option_expiry = datetime.strptime(ib_contract.lastTradeDateOrContractMonth, "%Y%m%d")
            contract_data.option_underlying = underlying_symbol + "_" + ib_contract.lastTradeDateOrContractMonth

        if contract_data.symbol not in self.contracts:
            self.gateway.on_contract(contract_data)
            self.contracts[contract_data.symbol] = contract_data

    def contractDetails(self, reqId: int, contractDetails: ContractDetails) -> None:
        # Handle contract details from IB
        super().contractDetails(reqId, contractDetails)
        self.convert_contract_detail(reqId, contractDetails)

    def contractDetailsEnd(self, reqId: int) -> None:
        # Handle contract details end notification
        super().contractDetailsEnd(reqId)

        underlying = self.reqid_underlying_map.get(reqId, None)
        if not underlying:
            return

        self.gateway.write_log(f"{underlying} option portfolio query complete", INFO)

        self.save_contract_data()

        option_contracts = [c for c in self.contracts.values() if c.product == Product.OPTION]
        self.gateway.write_log(
            f"Option portfolio query complete for {underlying}: "
            f"reqId={reqId}, underlying_symbol={underlying}, "
            f"contract_count={len(option_contracts)}, "
            f"total_contracts={len(self.contracts)}",
            INFO,
        )

    def query_portfolio(self, underlying_symbol: str) -> None:
        """Query contract details and full option chain for a given underlying (STK or IND)."""
        if not self.status:
            return

        symbol, currency, secType, exchange = underlying_symbol.split("-")

        ib_contract = Contract()
        ib_contract.symbol = symbol
        ib_contract.currency = currency or "USD"
        ib_contract.secType = secType
        ib_contract.exchange = "CBOE" if secType == "IND" else exchange

        # Query the underlying contract first
        try:
            self.reqid += 1
            self.reqContractDetails(self.reqid, ib_contract)
            self.gateway.write_log(f"Queried underlying contract: {symbol} ({secType})", DEBUG)
        except Exception as e:
            self.gateway.write_log(f"Contract query failed: {e}", ERROR)
            return

        # Query the option chain(s), simplify flow by separating STK and IND only
        if secType == "STK":
            ib_contract.secType = "OPT"
            ib_contract.exchange = "SMART"
            try:
                self.reqid += 1
                self.reqContractDetails(self.reqid, ib_contract)
                self.reqid_underlying_map[self.reqid] = underlying_symbol
                self.gateway.write_log(f"Start downloading option chain for {underlying_symbol}", DEBUG)
            except Exception as e:
                self.gateway.write_log(f"Option chain query failed: {e}", ERROR)

        elif secType == "IND":
            for tclass in ["SPX", "SPXW"]:
                opt_contract = Contract()
                opt_contract.symbol = symbol
                opt_contract.currency = currency
                opt_contract.secType = "OPT"
                opt_contract.exchange = "CBOE"
                opt_contract.tradingClass = tclass
                try:
                    self.reqid += 1
                    self.reqContractDetails(self.reqid, opt_contract)
                    self.reqid_underlying_map[self.reqid] = underlying_symbol
                    self.gateway.write_log(f"Start downloading {tclass} option chain for {underlying_symbol}", DEBUG)
                except Exception as e:
                    self.gateway.write_log(f"Option chain query failed for {tclass}: {e}", ERROR)

    # ====================
    # Data Management
    # ====================

    def save_contract_data(self) -> None:
        """Save contract data to database"""
        try:
            db_engine: Any = self.gateway.main_engine.db_engine
            if not db_engine:
                self.gateway.write_log("Database engine not found", ERROR)
                return

            for symbol, contract in self.contracts.items():
                c: ContractData = copy(contract)
                c.gateway_name = "IB"
                db_engine.save_contract_data(c, symbol)

            self.gateway.write_log("Contract data saved to database", INFO)

        except Exception as e:
            self.gateway.write_log(f"Database save failed: {e}", ERROR)

    # ====================
    # Utility Methods
    # ====================


def generate_formatted_symbol(ib_contract: Contract) -> str:
    # Generate formatted symbol from IB contract
    if not ib_contract.symbol or ib_contract.symbol.strip() == "":
        return str(ib_contract.conId) if ib_contract.conId else "UNKNOWN"

    fields = [ib_contract.symbol]

    if ib_contract.secType in ["FUT", "OPT", "FOP"]:
        fields.append(ib_contract.lastTradeDateOrContractMonth or "")

    if ib_contract.secType in ["OPT", "FOP"]:
        fields.append(ib_contract.right or "")
        fields.append(str(ib_contract.strike or 0))
        fields.append(str(ib_contract.multiplier or 100))

    fields.append(ib_contract.currency or "")
    fields.append(ib_contract.secType or "")

    formatted_symbol = JOIN_SYMBOL.join(fields)
    return formatted_symbol


def generate_ib_single_contract(symbol: str, trading_class: str | None = None) -> Contract | None:
    ib_contract: Contract = Contract()
    # Only consider option in the first if
    if "-" in symbol:
        try:
            fields: list = symbol.split(JOIN_SYMBOL)
            ib_contract.exchange = "SMART"
            ib_contract.secType = fields[-1]
            ib_contract.currency = fields[-2]
            ib_contract.symbol = fields[0]
            if trading_class:
                ib_contract.tradingClass = trading_class
            if ib_contract.secType == "OPT":
                ib_contract.lastTradeDateOrContractMonth = fields[1]
                ib_contract.right = fields[2]
                ib_contract.strike = float(fields[3])
                ib_contract.multiplier = int(fields[4])
        except (IndexError, ValueError):
            return None
    else:
        ib_contract.symbol = symbol
        ib_contract.secType = "STK"
        ib_contract.currency = "USD"
        if trading_class:
            ib_contract.tradingClass = trading_class
        ib_contract.exchange = "SMART"
    return ib_contract


def generate_ib_combo_contract(legs: list[Leg], trading_class: str | None = None) -> Contract:
    bag = Contract()
    bag.secType = "BAG"
    bag.currency = "USD"
    bag.exchange = "SMART"
    if trading_class:
        bag.tradingClass = trading_class

    for leg in legs:
        if leg.symbol and "-" in leg.symbol:
            bag.symbol = leg.symbol.split("-")[0]
            break

    bag.comboLegs = []
    for leg in legs:
        ib_leg = ComboLeg()
        ib_leg.conId = leg.con_id
        ib_leg.ratio = abs(leg.ratio)

        if leg.direction == Direction.LONG:
            ib_leg.action = "BUY"
        else:
            ib_leg.action = "SELL"

        ib_leg.exchange = "SMART"
        bag.comboLegs.append(ib_leg)

    return bag
