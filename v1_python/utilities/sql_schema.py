CREATE_CONTRACT_EQUITY_TABLE = """
    CREATE TABLE IF NOT EXISTS contract_equity (
        symbol TEXT PRIMARY KEY,
        exchange TEXT NOT NULL,
        name TEXT,
        product TEXT NOT NULL,
        size REAL NOT NULL,
        pricetick REAL NOT NULL,
        min_volume REAL NOT NULL,
        net_position INTEGER NOT NULL,
        history_data INTEGER NOT NULL,
        stop_supported INTEGER NOT NULL,
        gateway_name TEXT NOT NULL,
        con_id INTEGER NOT NULL,
        trading_class TEXT,
        max_volume REAL,
        extra TEXT
    )
"""

CREATE_CONTRACT_OPTION_TABLE = """
    CREATE TABLE IF NOT EXISTS contract_option (
        symbol TEXT PRIMARY KEY,
        exchange TEXT NOT NULL,
        name TEXT,
        product TEXT NOT NULL,
        size REAL NOT NULL,
        pricetick REAL NOT NULL,
        min_volume REAL NOT NULL,
        net_position INTEGER NOT NULL,
        history_data INTEGER NOT NULL,
        stop_supported INTEGER NOT NULL,
        gateway_name TEXT NOT NULL,
        con_id INTEGER NOT NULL,
        trading_class TEXT,
        max_volume REAL,
        extra TEXT,
        portfolio TEXT,
        type TEXT,
        strike REAL,
        strike_index TEXT,
        expiry TEXT,
        underlying TEXT
    )
"""


CREATE_ORDERS_TABLE = """
    CREATE TABLE IF NOT EXISTS orders (
        timestamp TEXT NOT NULL,
        strategy_name TEXT NOT NULL,
        orderid TEXT PRIMARY KEY,
        symbol TEXT NOT NULL,
        exchange TEXT NOT NULL,
        trading_class TEXT,
        type TEXT NOT NULL,
        direction TEXT,
        price REAL NOT NULL,
        volume REAL NOT NULL,
        traded REAL NOT NULL,
        status TEXT NOT NULL,
        datetime TEXT,
        reference TEXT,
        is_combo INTEGER,
        legs_info TEXT
    )
"""

CREATE_TRADES_TABLE = """
    CREATE TABLE IF NOT EXISTS trades (
        timestamp TEXT NOT NULL,
        strategy_name TEXT NOT NULL,
        tradeid TEXT PRIMARY KEY,
        symbol TEXT NOT NULL,
        exchange TEXT NOT NULL,
        orderid TEXT NOT NULL,
        direction TEXT,
        price REAL NOT NULL,
        volume REAL NOT NULL,
        datetime TEXT
    )
"""
