import threading
import time
from datetime import datetime
from os import getenv
from typing import TYPE_CHECKING

import requests
from dotenv import find_dotenv, load_dotenv

from engines.engine_event import Event, EventEngine
from engines.engine_log import ERROR, INFO, WARNING
from utilities.base_engine import BaseEngine
from utilities.constant import Exchange, Product
from utilities.event import EVENT_CONTRACT, EVENT_LOG
from utilities.object import ChainMarketData, ContractData, LogData, OptionMarketData, TickData
from utilities.portfolio import InstrumentData, OptionData, PortfolioData, UnderlyingData

if TYPE_CHECKING:
    from engines.engine_main import MainEngine

# Load environment variables
_ = load_dotenv(find_dotenv())
load_dotenv()

# Tradier API constants
TRADIER_BASE_URL = "https://api.tradier.com/v1/"
TRADIER_TOKEN = getenv("TRADIER_TOKEN")
TRADIER_HEADERS = {"Authorization": f"Bearer {TRADIER_TOKEN}", "Accept": "application/json"}


APP_NAME = "MarketData"

# Rate limit constant (requests per minute)
RATE_LIMIT_PER_MINUTE: int = 120


class MarketDataEngine(BaseEngine):
    """Independent market data provider for market data"""

    def __init__(self, main_engine: "MainEngine", event_engine: EventEngine) -> None:
        super().__init__(main_engine, event_engine, APP_NAME)

        self.headers = TRADIER_HEADERS
        self.base_url = TRADIER_BASE_URL

        self.portfolios: dict[str, PortfolioData] = {}
        self.instruments: dict[str, InstrumentData] = {}
        self.contracts: dict[str, ContractData] = {}

        self.active_chains: dict[str, set[str]] = {}
        self.strategy_chains: dict[str, set[str]] = {}

        self.event_engine.register(EVENT_CONTRACT, self.process_contract_event)

        # Background polling control
        self.started: bool = False
        self.poll_thread: threading.Thread | None = None

    def _poll_interval(self) -> float:
        """Compute polling idle based on configured API rate limit constant."""
        rate = max(1, int(RATE_LIMIT_PER_MINUTE))
        return max(0.01, 60.0 / float(rate))

    # ---------------- Contract and Chain Management -------------------------

    def process_contract_event(self, event: Event) -> None:
        """Process contract events to maintain data containers"""
        contract: ContractData = event.data

        self.contracts[contract.symbol] = contract
        underlying_name = contract.symbol.split("-")[0]
        portfolio = self.get_or_create_portfolio(underlying_name)

        if contract.product in (Product.EQUITY, Product.INDEX):
            portfolio.set_underlying(contract)
            underlying = portfolio.underlying
            self.instruments[underlying.symbol] = underlying

        if contract.product == Product.OPTION:
            portfolio.add_option(contract)
            option = portfolio.options[contract.symbol]
            self.instruments[contract.symbol] = option

    def subscribe_chains(self, strategy_name: str, chain_symbols: list[str]) -> None:
        """Subscribe a strategy to chain updates"""
        if strategy_name not in self.strategy_chains:
            self.strategy_chains[strategy_name] = set()

        for chain_symbol in chain_symbols:
            self.strategy_chains[strategy_name].add(chain_symbol)

            portfolio_name = chain_symbol.split("_")[0]
            if portfolio_name not in self.active_chains:
                self.active_chains[portfolio_name] = set()
            self.active_chains[portfolio_name].add(chain_symbol)

        self.write_log(f"Strategy {strategy_name} subscribed to chains: {chain_symbols}", INFO)

    def unsubscribe_chains(self, strategy_name: str) -> None:
        """Unsubscribe a strategy from all chains"""
        chains = self.strategy_chains.pop(strategy_name, set())

        for chain_symbol in chains:
            portfolio_name = chain_symbol.split("_")[0]
            if portfolio_name in self.active_chains:
                self.active_chains[portfolio_name].discard(chain_symbol)
                if not self.active_chains[portfolio_name]:
                    self.active_chains.pop(portfolio_name)

        self.write_log(f"Strategy {strategy_name} unsubscribed from all chains", INFO)

    # ---------------- Portfolio Management ---------------------------------

    def get_or_create_portfolio(self, portfolio_name: str) -> PortfolioData:
        """Get existing portfolio or create new one - purely for data organization"""
        portfolio = self.portfolios.get(portfolio_name)
        if not portfolio:
            portfolio = PortfolioData(portfolio_name, self.event_engine)
            self.portfolios[portfolio_name] = portfolio

        return portfolio

    def get_portfolio(self, portfolio_name: str) -> PortfolioData | None:
        """Get portfolio data container by name"""
        return self.portfolios.get(portfolio_name)

    def get_all_portfolio_names(self) -> list[str]:
        """Get all portfolio names"""
        return list(self.portfolios.keys())

    def get_instrument(self, symbol: str) -> InstrumentData | OptionData | UnderlyingData | None:
        """Get instrument data by symbol"""
        return self.instruments.get(symbol)

    def get_contract(self, symbol: str) -> ContractData | None:
        """Get raw contract by symbol"""
        return self.contracts.get(symbol)

    def get_all_contracts(self) -> list[ContractData]:
        return list(self.contracts.values())

    # ---------------- Market Data Management --------------------------------

    def start_market_data_update(self) -> None:
        """Start background polling loop if not already running."""
        if self.started and self.poll_thread and self.poll_thread.is_alive():
            return

        self.started = True
        self.poll_thread = threading.Thread(target=self._poll_market_data_loop, name="MarketDataPolling", daemon=True)
        self.poll_thread.start()
        self.write_log("Market data update started", INFO)

    def stop_market_data_update(self) -> None:
        """Stop background polling loop and join the thread."""
        self.started = False
        try:
            if self.poll_thread and self.poll_thread.is_alive():
                self.poll_thread.join(timeout=2)
        except Exception as e:
            self.write_log(f"Error stopping market data polling thread: {e}", WARNING)

    def _poll_market_data_loop(self) -> None:
        """Continuously poll market data for all subscribed chains, respecting API limits."""
        while self.started:
            try:
                if not self.active_chains:
                    time.sleep(self._poll_interval())
                    continue

                portfolios_to_chains = {k: set(v) for k, v in self.active_chains.items() if v}

                for portfolio_name, chain_set in portfolios_to_chains.items():
                    portfolio = self.get_portfolio(portfolio_name)
                    if not portfolio:
                        continue

                    chains_data = self._fetch_option_chain_ticks(list(chain_set))
                    if chains_data:
                        self.inject_option_chain_market_data(chains_data, portfolio)

                    underlying_symbol: str = portfolio.underlying.symbol
                    underlying_data = self._fetch_underlying_tick(underlying_symbol)
                    if underlying_data:
                        self.inject_underlying_tick(underlying_data, portfolio)

                time.sleep(self._poll_interval())
            except Exception as e:
                self.write_log(f"Polling loop error: {e}", ERROR)
                time.sleep(0.5)

    def inject_option_chain_market_data(self, chains: list[dict], portfolio: PortfolioData) -> None:
        """Inject option chain market data into portfolio"""
        for chain in chains:
            chain_symbol = chain["expiration"]
            chain_market_data = ChainMarketData(
                chain_symbol=chain_symbol,
                datetime=datetime.now(),
                underlying_symbol=(portfolio.underlying.symbol if portfolio.underlying else ""),
                underlying_last=(portfolio.underlying.mid_price if portfolio.underlying else 0.0),
                gateway_name="TRADIER",
            )

            for opt in chain.get("options", []):
                symbol = opt["symbol"]

                bid = round(float(opt.get("bid", 0) or 0), 2)
                ask = round(float(opt.get("ask", 0) or 0), 2)
                last = round(float(opt.get("last", 0) or 0), 2)
                if not last and (bid or ask):
                    last = round((bid + ask) / 2, 2) if bid and ask else bid or ask

                greeks = opt.get("greeks", {})
                option_data = OptionMarketData(
                    symbol=symbol,
                    exchange=Exchange.SMART,
                    datetime=datetime.now(),
                    bid_price=bid,
                    ask_price=ask,
                    last_price=last,
                    volume=float(opt.get("volume", 0) or 0),
                    open_interest=float(opt.get("open_interest", 0) or 0),
                    delta=round(float(greeks.get("delta", 0.0) or 0.0), 4),
                    gamma=round(float(greeks.get("gamma", 0.0) or 0.0), 4),
                    theta=round(float(greeks.get("theta", 0.0) or 0.0), 4),
                    vega=round(float(greeks.get("vega", 0.0) or 0.0), 4),
                    mid_iv=round(float(greeks.get("mid_iv", 0.0) or 0.0), 4),
                    gateway_name="TRADIER",
                )
                chain_market_data.add_option(option_data)

            chain_data = portfolio.chains.get(chain_symbol)
            if chain_data:
                chain_data.update_option_chain(chain_market_data)
            else:
                self.write_log(f"No chain data found for {chain_symbol} in portfolio {portfolio.name}", WARNING)

    def inject_underlying_tick(self, underlying_data: dict, portfolio: PortfolioData) -> None:
        """Inject underlying tick into portfolio"""
        symbol: str = underlying_data["symbol"]
        bid: float = underlying_data["bid"]
        ask: float = underlying_data["ask"]
        last: float = (bid + ask) / 2 if bid and ask else bid or ask

        tick = TickData(
            symbol=symbol,
            exchange=Exchange.SMART,
            datetime=datetime.now(),
            bid_price_1=bid,
            ask_price_1=ask,
            last_price=last,
            gateway_name="TRADIER",
        )

        portfolio.update_underlying_tick(tick)

    def _fetch_option_chain_ticks(self, chain_keys: list[str]) -> list[dict]:
        """Fetch option chain data from Tradier API"""
        result = []

        for chain_key in chain_keys:
            try:
                symbol, date_part = chain_key.split("_")
                expiration = f"{date_part[:4]}-{date_part[4:6]}-{date_part[6:8]}"
            except Exception:
                self.write_log(f"Invalid chain_key: {chain_key}", ERROR)
                continue

            url = f"{self.base_url}markets/options/chains"
            params = {"symbol": symbol, "expiration": expiration, "greeks": "true"}

            try:
                response = requests.get(url, headers=self.headers, params=params, timeout=10)
                data = response.json()
                options = data.get("options", {}).get("option", [])

                formatted_options = []
                for opt in options:
                    root = opt.get("root_symbol") or opt.get("underlying")
                    strike = opt.get("strike")
                    opt_type = opt.get("option_type")[0].upper()
                    contract_size = opt.get("contract_size", 100)

                    # Map SPXW to SPX for symbol matching with stored contracts
                    # Extract trading class from chain_key (e.g., "SPXW" from "SPXW_20251024")
                    chain_trading_class = chain_key.split("_")[0]
                    mapped_symbol = "SPX" if chain_trading_class in ["SPX", "SPXW"] else root
                    formatted_symbol = f"{mapped_symbol}-{expiration.replace('-', '')}-{opt_type}-{strike}-{contract_size}-USD-OPT"

                    greeks_data = opt.get("greeks", {})

                    formatted_options.append(
                        {
                            "symbol": formatted_symbol,
                            "bid": opt.get("bid"),
                            "ask": opt.get("ask"),
                            "last": opt.get("last"),
                            "volume": opt.get("volume"),
                            "open_interest": opt.get("open_interest"),
                            "greeks": {
                                "delta": greeks_data.get("delta", 0.0),
                                "gamma": greeks_data.get("gamma", 0.0),
                                "theta": greeks_data.get("theta", 0.0),
                                "vega": greeks_data.get("vega", 0.0),
                                "mid_iv": greeks_data.get("mid_iv", 0.0),
                            },
                        }
                    )

                result.append({"expiration": chain_key, "options": formatted_options})

            except Exception as e:
                self.write_log(f"Error fetching chain {chain_key}: {e}", ERROR)
                continue

        return result

    def _fetch_underlying_tick(self, symbol: str) -> dict:
        """Fetch underlying tick data from Tradier API"""
        try:
            root_symbol = symbol.split("-")[0]
        except Exception:
            self.write_log(f"Invalid symbol: {symbol}", ERROR)
            return {}

        url = f"{self.base_url}markets/quotes"
        params = {"symbols": root_symbol}

        try:
            response = requests.get(url, headers=self.headers, params=params, timeout=10)
            data = response.json()
            quote = data.get("quotes", {}).get("quote", {})

            return {"symbol": symbol, "bid": quote.get("bid", 0) or 0, "ask": quote.get("ask", 0) or 0}
        except Exception as e:
            self.write_log(f"Error fetching tick for {symbol}: {e}", ERROR)
            return {}

    def write_log(self, msg: str, level: int = INFO) -> None:
        """Write log to event system"""
        log = LogData(msg=msg, level=level, gateway_name=self.engine_name)
        event = Event(EVENT_LOG, log)
        self.event_engine.put(event)
