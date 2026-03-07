from datetime import datetime
from math import log

from engines.engine_event import EventEngine
from utilities.constant import Exchange, OptionType
from utilities.object import ChainMarketData, ContractData, TickData
from utilities.utility import ANNUAL_DAYS, calculate_days_to_expiry


class InstrumentData:
    def __init__(self, contract: ContractData) -> None:
        self.symbol: str = contract.symbol
        self.exchange: Exchange = contract.exchange
        self.size: float = contract.size
        self.mid_price: float = 0.0
        self.tick: TickData | None = None
        self.portfolio: PortfolioData

    def set_portfolio(self, portfolio: "PortfolioData") -> None:
        self.portfolio = portfolio


class OptionData(InstrumentData):
    def __init__(self, contract: ContractData) -> None:
        super().__init__(contract)
        # Allow None for optional contract fields
        self.strike_price: float | None = contract.option_strike
        self.chain_index: str | None = contract.option_index
        self.option_type: int = 1 if contract.option_type == OptionType.CALL else -1
        self.option_expiry: datetime | None = contract.option_expiry
        self.underlying: UnderlyingData
        self.chain: ChainData
        self.delta: float = 0
        self.gamma: float = 0
        self.theta: float = 0
        self.vega: float = 0
        self.mid_iv: float = 0

    def set_chain(self, chain: "ChainData") -> None:
        self.chain = chain

    def set_underlying(self, underlying: "UnderlyingData") -> None:
        self.underlying = underlying

    # New snapshot IV utility methods
    def moneyness(self, use_log: bool = False) -> float | None:
        """Return S/K or ln(S/K). Minimal guards."""
        s = self.underlying.mid_price
        k = self.strike_price
        if k is None or k == 0:
            return None
        ratio = s / k
        if use_log:
            if ratio <= 0:
                return None
            return log(ratio)
        return ratio

    def is_otm(self) -> bool:
        """True if option is out-of-the-money."""
        s = self.underlying.mid_price
        k = self.strike_price
        if k is None:
            return False
        if self.option_type > 0:  # Call
            return k > s
        else:  # Put
            return k < s


class UnderlyingData(InstrumentData):
    def __init__(self, contract: ContractData) -> None:
        super().__init__(contract)
        self.theo_delta: float = self.size
        self.chains: dict[str, ChainData] = {}

    def add_chain(self, chain: "ChainData") -> None:
        self.chains[chain.chain_symbol] = chain

    def update_underlying_tick(self, tick: TickData) -> None:
        self.tick = tick
        self.mid_price = (tick.bid_price_1 + tick.ask_price_1) / 2


class ChainData:
    def __init__(self, chain_symbol: str, event_engine: EventEngine) -> None:
        self.chain_symbol: str = chain_symbol
        self.event_engine: EventEngine = event_engine
        self.underlying: UnderlyingData | None = None
        self.options: dict[str, OptionData] = {}
        self.calls: dict[str, OptionData] = {}
        self.puts: dict[str, OptionData] = {}
        self.portfolio: PortfolioData
        self.indexes: list[str] = []
        self.atm_price: float = 0
        self.atm_index: str = ""
        self.days_to_expiry: int = 0
        self.time_to_expiry: float = 0

    def add_option(self, option: OptionData) -> None:
        self.options[option.symbol] = option
        if option.chain_index is not None:
            if option.option_type > 0:
                self.calls[option.chain_index] = option
            else:
                self.puts[option.chain_index] = option
        option.set_chain(self)

        if option.chain_index is not None and option.chain_index not in self.indexes:
            self.indexes.append(option.chain_index)
            try:
                float(option.chain_index)
                self.indexes.sort(key=float)
            except ValueError:
                self.indexes.sort()

        if not self.days_to_expiry:
            self.days_to_expiry = calculate_days_to_expiry(option.option_expiry)
            self.time_to_expiry = self.days_to_expiry / ANNUAL_DAYS

    def update_option_chain(self, market_data: ChainMarketData) -> None:
        if self.underlying is not None:
            self.underlying.mid_price = market_data.underlying_last

        for symbol, option_data in market_data.options.items():
            if symbol in self.options:
                option = self.options[symbol]
                option.mid_price = option_data.last_price
                option.delta = option_data.delta * option.size
                option.gamma = option_data.gamma * option.size
                option.theta = option_data.theta * option.size
                option.vega = option_data.vega * option.size
                option.mid_iv = option_data.mid_iv

        self.calculate_atm_price()

    def set_underlying(self, underlying: "UnderlyingData") -> None:
        underlying.add_chain(self)
        self.underlying = underlying
        for option in self.options.values():
            option.set_underlying(underlying)

    def set_portfolio(self, portfolio: "PortfolioData") -> None:
        self.portfolio = portfolio

    def calculate_atm_price(self) -> None:
        # Collect available strikes from calls and puts
        strike_entries: list[tuple[float, str]] = []  # (strike, index)
        seen_indexes: set[str] = set()

        for index, opt in self.calls.items():
            if opt.strike_price is not None and index not in seen_indexes:
                strike_entries.append((opt.strike_price, index))
                seen_indexes.add(index)

        for index, opt in self.puts.items():
            if opt.strike_price is not None and index not in seen_indexes:
                strike_entries.append((opt.strike_price, index))
                seen_indexes.add(index)

        if not strike_entries:
            # No strikes available
            self.atm_price = 0
            self.atm_index = ""
            return

        # Prefer using current underlying price if available; otherwise use median strike
        underlying_price: float | None = None
        if self.underlying and self.underlying.mid_price:
            underlying_price = self.underlying.mid_price

        selected_strike: float
        selected_index: str

        if underlying_price is not None and underlying_price > 0:
            # Choose strike closest to underlying
            selected_strike, selected_index = min(strike_entries, key=lambda t: abs(t[0] - underlying_price))
        else:
            # Fallback: choose median strike by value
            strike_entries.sort(key=lambda t: t[0])
            mid = len(strike_entries) // 2
            selected_strike, selected_index = strike_entries[mid]

        self.atm_price = selected_strike
        self.atm_index = selected_index

    # New snapshot IV analytics methods
    def get_atm_iv(self) -> float | None:
        """Return ATM IV using ATM call if available, else ATM put."""
        index = self.atm_index
        if not index:
            return None
        call = self.calls.get(index)
        put = self.puts.get(index)
        if call is not None and call.mid_iv is not None:
            return call.mid_iv
        if put is not None and put.mid_iv is not None:
            return put.mid_iv
        return None

    def best_iv(self, options: dict, target: float) -> float | None:
        best = None
        min_diff = float("inf")
        for opt in options.values():
            if opt.mid_iv is None or not opt.is_otm():
                continue
            size = opt.size if opt.size else 0
            d = (opt.delta / size) if size else opt.delta
            if d is None:
                continue
            diff = abs(abs(d) - target)
            if diff < min_diff:
                min_diff = diff
                best = opt.mid_iv
        return best

    def get_skew(self, delta: float = 25) -> float | None:
        """Return IV_call_25Δ / IV_put_25Δ."""
        target = delta / 100.0
        call_iv = self.best_iv(self.calls, target)
        put_iv = self.best_iv(self.puts, target)
        if call_iv is None or put_iv is None or put_iv == 0:
            return None
        return call_iv / put_iv


class PortfolioData:
    def __init__(self, name: str, event_engine: EventEngine) -> None:
        self.name: str = name
        self.event_engine: EventEngine = event_engine
        self.options: dict[str, OptionData] = {}
        self.chains: dict[str, ChainData] = {}
        self.underlying: UnderlyingData

    def update_option_chain(self, market_data: ChainMarketData) -> None:
        chain = self.chains.get(market_data.chain_symbol)
        if chain:
            chain.update_option_chain(market_data)

    def update_underlying_tick(self, tick: TickData) -> None:
        if self.underlying and tick.symbol == self.underlying.symbol:
            self.underlying.update_underlying_tick(tick)

    def set_underlying(self, contract: ContractData) -> None:
        self.underlying = UnderlyingData(contract)
        self.underlying.set_portfolio(self)
        self.underlying_symbol = contract.symbol

        for chain in self.chains.values():
            chain.set_underlying(self.underlying)

    def get_chain(self, chain_symbol: str) -> ChainData:
        chain: ChainData | None = self.chains.get(chain_symbol, None)
        if not chain:
            chain = ChainData(chain_symbol, self.event_engine)
            chain.set_portfolio(self)
            self.chains[chain_symbol] = chain
        return chain

    def get_chain_by_expiry(self, min_dte: int, max_dte: int) -> list[str]:
        matching_chains = []

        for chain_symbol, chain in self.chains.items():
            if min_dte <= chain.days_to_expiry <= max_dte:
                matching_chains.append(chain_symbol)

        return matching_chains

    def add_option(self, contract: ContractData) -> None:
        option = OptionData(contract)
        option.set_portfolio(self)
        self.options[contract.symbol] = option

        parts = contract.symbol.split("-")

        underlying_name = parts[0]
        expiry_str = parts[1]
        chain_symbol = f"{underlying_name}_{expiry_str}"

        chain = self.get_chain(chain_symbol)
        chain.add_option(option)

    def calculate_atm_price(self) -> None:
        for chain in self.chains.values():
            chain.calculate_atm_price()
