#!/usr/bin/env python3
"""
Standalone script: query contract details and option chain from IB TWS using ib_insync.
Writes all contracts to PostgreSQL (same schema as Otrader engine_db_pg).

Connection string: load from .env as DATABASE_URL (via python-dotenv).

Usage:
  python query_contracts_ibinsync.py

Requirements: pip install ib_insync python-dotenv psycopg2-binary
"""
from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass

try:
    from dotenv import load_dotenv
except ImportError:
    load_dotenv = None

# TWS connection (hardcoded)
CONFIG = {
    "host": "127.0.0.1",
    "port": 7497,
    "client_id": 1,
    "underlying_symbol": "SPX-USD-IND",
}

try:
    from ib_insync import IB, Contract, ContractDetails, Index
except ImportError:
    print("ib_insync is required: pip install ib_insync", file=sys.stderr)
    sys.exit(1)

try:
    import psycopg2
except ImportError:
    psycopg2 = None

# DDL aligned with Otrader/infra/db/engine_db_pg.cpp
CREATE_CONTRACT_EQUITY = """
CREATE TABLE IF NOT EXISTS contract_equity (
    symbol TEXT PRIMARY KEY, exchange TEXT NOT NULL, name TEXT, product TEXT NOT NULL,
    size DOUBLE PRECISION NOT NULL, pricetick DOUBLE PRECISION NOT NULL, min_volume DOUBLE PRECISION NOT NULL,
    net_position INTEGER NOT NULL, history_data INTEGER NOT NULL, stop_supported INTEGER NOT NULL,
    gateway_name TEXT NOT NULL, con_id INTEGER, trading_class TEXT, max_volume DOUBLE PRECISION, extra TEXT)
"""
CREATE_CONTRACT_OPTION = """
CREATE TABLE IF NOT EXISTS contract_option (
    symbol TEXT PRIMARY KEY, exchange TEXT NOT NULL, name TEXT, product TEXT NOT NULL,
    size DOUBLE PRECISION NOT NULL, pricetick DOUBLE PRECISION NOT NULL, min_volume DOUBLE PRECISION NOT NULL,
    net_position INTEGER NOT NULL, history_data INTEGER NOT NULL, stop_supported INTEGER NOT NULL,
    gateway_name TEXT NOT NULL, con_id INTEGER, trading_class TEXT, max_volume DOUBLE PRECISION, extra TEXT,
    portfolio TEXT, type TEXT, strike DOUBLE PRECISION, strike_index TEXT, expiry TEXT, underlying TEXT)
"""

# Symbol join used by gateway (object.py / constant.py JOIN_SYMBOL)
JOIN_SYMBOL = "-"

# Minimal contract record equivalent to gateway ContractData (for output)
@dataclass
class ContractRecord:
    symbol: str
    exchange: str
    name: str
    product: str  # STK, OPT, IND, ...
    size: float
    pricetick: float
    con_id: int | None = None
    trading_class: str | None = None
    # option-specific
    option_strike: float | None = None
    option_underlying: str | None = None
    option_type: str | None = None  # C, P
    option_expiry: str | None = None  # YYYYMMDD
    option_portfolio: str | None = None
    option_index: str | None = None


def format_contract_symbol(contract: Contract) -> str:
    """Build platform symbol string from IB contract (same idea as generate_formatted_symbol)."""
    if not contract.symbol or contract.symbol.strip() == "":
        return str(contract.conId) if contract.conId else "UNKNOWN"
    root = contract.symbol
    if contract.secType in ("OPT", "FOP") and root.upper() == "SPX":
        root = "SPXW"
    fields = [root]
    if contract.secType in ("FUT", "OPT", "FOP"):
        fields.append(contract.lastTradeDateOrContractMonth or "")
    if contract.secType in ("OPT", "FOP"):
        fields.append(contract.right or "")
        fields.append(str(contract.strike or 0))
        fields.append(str(contract.multiplier or 100))
    fields.append(contract.currency or "USD")
    fields.append(contract.secType or "")
    return JOIN_SYMBOL.join(fields)


def contract_detail_to_record(detail: ContractDetails, gateway_name: str = "IB") -> ContractRecord:
    """Convert IB ContractDetails to ContractRecord (equivalent to gateway convert_contract_detail)."""
    c = detail.contract
    mult = float(c.multiplier or 1)
    symbol = format_contract_symbol(c)
    rec = ContractRecord(
        symbol=symbol,
        exchange=c.exchange or "SMART",
        name=detail.longName or symbol,
        product=c.secType or "UNKNOWN",
        size=mult,
        pricetick=detail.minTick or 0.01,
        con_id=c.conId,
        trading_class=c.tradingClass or None,
    )
    if c.secType == "OPT":
        underlying_symbol = str(detail.underConId) if detail.underConId else (c.symbol or "")
        rec.option_portfolio = underlying_symbol + "_O"
        rec.option_type = c.right  # C or P
        rec.option_strike = c.strike
        rec.option_index = str(c.strike) if c.strike is not None else None
        rec.option_expiry = c.lastTradeDateOrContractMonth  # YYYYMMDD
        rec.option_underlying = underlying_symbol + "_" + (c.lastTradeDateOrContractMonth or "")
    return rec


def expiry_to_date(yyyymmdd: str | None) -> str:
    """Convert IB expiry YYYYMMDD to YYYY-MM-DD for PG (matches C++ date_to_str)."""
    if not yyyymmdd or len(yyyymmdd) != 8:
        return ""
    return f"{yyyymmdd[:4]}-{yyyymmdd[4:6]}-{yyyymmdd[6:8]}"


def option_type_to_string(r: str | None) -> str:
    """C/P -> CALL/PUT for PG (matches C++ to_string(OptionType))."""
    if r == "P":
        return "PUT"
    return "CALL" if r == "C" else ""


def product_for_db(product: str) -> str:
    """Map IB secType to DB product (C++ product_from_string)."""
    if product == "IND":
        return "INDEX"
    if product == "OPT":
        return "OPTION"
    if product == "STK":
        return "EQUITY"
    return product or "UNKNOWN"


def save_to_pg(records: list[ContractRecord], conninfo: str) -> None:
    """Write all records to PostgreSQL (contract_equity / contract_option). Schema matches engine_db_pg.cpp."""
    if not psycopg2:
        raise RuntimeError("psycopg2 is required: pip install psycopg2-binary")
    conn = psycopg2.connect(conninfo)
    try:
        with conn.cursor() as cur:
            cur.execute(CREATE_CONTRACT_EQUITY)
            cur.execute(CREATE_CONTRACT_OPTION)
        conn.commit()

        with conn.cursor() as cur:
            for r in records:
                product = product_for_db(r.product)
                con_id = r.con_id if r.con_id is not None else 0
                trading_class = r.trading_class or ""
                if r.product == "OPT":
                    opt_type = option_type_to_string(r.option_type)
                    strike = float(r.option_strike or 0)
                    strike_index = r.option_index or ""
                    expiry = expiry_to_date(r.option_expiry)
                    portfolio = r.option_portfolio or ""
                    underlying = r.option_underlying or ""
                    cur.execute(
                        """
                        INSERT INTO contract_option (
                            symbol, exchange, name, product, size, pricetick, min_volume,
                            net_position, history_data, stop_supported, gateway_name, con_id,
                            trading_class, max_volume, extra,
                            portfolio, type, strike, strike_index, expiry, underlying)
                        VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, NULL, %s, %s, %s, %s, %s, %s)
                        ON CONFLICT (symbol) DO UPDATE SET
                            exchange=EXCLUDED.exchange, name=EXCLUDED.name, product=EXCLUDED.product,
                            size=EXCLUDED.size, pricetick=EXCLUDED.pricetick, min_volume=EXCLUDED.min_volume,
                            net_position=EXCLUDED.net_position, history_data=EXCLUDED.history_data,
                            stop_supported=EXCLUDED.stop_supported, gateway_name=EXCLUDED.gateway_name,
                            con_id=EXCLUDED.con_id, trading_class=EXCLUDED.trading_class,
                            max_volume=EXCLUDED.max_volume,
                            portfolio=EXCLUDED.portfolio, type=EXCLUDED.type, strike=EXCLUDED.strike,
                            strike_index=EXCLUDED.strike_index, expiry=EXCLUDED.expiry, underlying=EXCLUDED.underlying
                        """,
                        (
                            r.symbol, r.exchange, r.name, product, r.size, r.pricetick,
                            0.0, 0, 0, 0, "IB", con_id, trading_class, 0.0,
                            portfolio, opt_type, strike, strike_index, expiry, underlying,
                        ),
                    )
                else:
                    cur.execute(
                        """
                        INSERT INTO contract_equity (
                            symbol, exchange, name, product, size, pricetick, min_volume,
                            net_position, history_data, stop_supported, gateway_name, con_id,
                            trading_class, max_volume, extra)
                        VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, NULL)
                        ON CONFLICT (symbol) DO UPDATE SET
                            exchange=EXCLUDED.exchange, name=EXCLUDED.name, product=EXCLUDED.product,
                            size=EXCLUDED.size, pricetick=EXCLUDED.pricetick, min_volume=EXCLUDED.min_volume,
                            net_position=EXCLUDED.net_position, history_data=EXCLUDED.history_data,
                            stop_supported=EXCLUDED.stop_supported, gateway_name=EXCLUDED.gateway_name,
                            con_id=EXCLUDED.con_id, trading_class=EXCLUDED.trading_class, max_volume=EXCLUDED.max_volume
                        """,
                        (
                            r.symbol, r.exchange, r.name, product, r.size, r.pricetick,
                            0.0, 0, 0, 0, "IB", con_id, trading_class, 0.0,
                        ),
                    )
        conn.commit()
    finally:
        conn.close()


def parse_underlying(underlying_symbol: str) -> tuple[str, str, str, str]:
    """Parse underlying_symbol as symbol-currency-secType-exchange (same as gateway query_portfolio)."""
    parts = underlying_symbol.split("-")
    if len(parts) < 3:
        raise ValueError(
            f"underlying_symbol must be 'SYMBOL-CURRENCY-SECTYPE' or 'SYMBOL-CURRENCY-SECTYPE-EXCHANGE', got {underlying_symbol!r}"
        )
    symbol = parts[0]
    currency = parts[1] or "USD"
    sec_type = parts[2]
    exchange = parts[3] if len(parts) > 3 else ("CBOE" if sec_type == "IND" else "SMART")
    return symbol, currency, sec_type, exchange


def run_query(underlying_symbol: str) -> list[ContractRecord]:
    """
    Connect to TWS, request index underlying + option chain (SPXW only). Index only, no stock.
    """
    symbol, currency, sec_type, exchange = parse_underlying(underlying_symbol)
    if sec_type != "IND":
        raise ValueError(f"Only IND (index) underlying is supported, got secType={sec_type!r}")

    host = CONFIG["host"]
    port = CONFIG["port"]
    ib = IB()
    ib.connect(host, port, clientId=CONFIG["client_id"])
    all_records: list[ContractRecord] = []

    try:
        # 1) Index underlying (ib_insync: Index(symbol, exchange, currency))
        underlying = Index(symbol, exchange, currency)
        underlying_details = ib.reqContractDetails(underlying)
        for d in underlying_details:
            all_records.append(contract_detail_to_record(d))

        # 2) Option chain (SPXW only)
        tclass = "SPXW"
        opt = Contract()
        opt.symbol = symbol
        opt.currency = currency
        opt.secType = "OPT"
        opt.exchange = "CBOE"
        opt.tradingClass = tclass
        opt_details = ib.reqContractDetails(opt)
        for d in opt_details:
            all_records.append(contract_detail_to_record(d))
    finally:
        ib.disconnect()
    return all_records


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Query IB contracts (SPXW), write all to PostgreSQL."
    )
    parser.parse_args()

    if load_dotenv is not None:
        load_dotenv()
    conninfo = os.environ.get("DATABASE_URL", "").strip()
    if not conninfo:
        print("DATABASE_URL not set. Create a .env with DATABASE_URL=postgresql://...", file=sys.stderr)
        sys.exit(1)

    underlying_symbol = CONFIG["underlying_symbol"]
    try:
        records = run_query(underlying_symbol)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    if not records:
        sys.exit(1)

    try:
        save_to_pg(records, conninfo)
    except Exception as e:
        print(f"Error writing to PostgreSQL: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
