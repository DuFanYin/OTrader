import sys
from pathlib import Path

# Add project root to Python path for absolute imports
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from time import sleep

import pandas as pd

from engines.engine_event import EventEngine
from engines.engine_main import MainEngine


def option_chain_iv_greeks_df(portfolio, chain_id: str):
    """Generate a formatted DataFrame of option chain data with IV and Greeks."""
    columns = [
        "DTE", "Mid Px", "IV", "Delta", "Gamma", "Theta", "",
        "Strike", "", "Mid Px", "IV", "Delta", "Gamma", "Theta"
    ]

    chain = portfolio.chains.get(chain_id)
    if not chain:
        return pd.DataFrame([["❌ Chain Not Found"] + [""] * (len(columns) - 1)], columns=columns)
    
    # Group options by strike
    strike_groups = {}
    for option in chain.options.values():
        strike = option.strike_price
        if strike not in strike_groups:
            strike_groups[strike] = {"call": None, "put": None}
        strike_groups[strike]["call" if option.option_type == 1 else "put"] = option

    dte = chain.days_to_expiry
    data = []

    for strike in sorted(strike_groups.keys()):
        group = strike_groups[strike]
        row = [f"{dte}"]

        # Call side
        if group["call"]:
            call = group["call"]
            row.extend([
                f"{call.mid_price:.2f}" if call.mid_price else "",
                f"{call.mid_iv:.4f}", f"{call.delta:.4f}",
                f"{call.gamma:.4f}", f"{call.theta:.4f}"
            ])
        else:
            row.extend([""] * 5)

        row.extend([" ", f"{strike:.2f}", " "])

        # Put side
        if group["put"]:
            put = group["put"]
            row.extend([
                f"{put.mid_price:.2f}" if put.mid_price else "",
                f"{put.mid_iv:.4f}", f"{put.delta:.4f}",
                f"{put.gamma:.4f}", f"{put.theta:.4f}"
            ])
        else:
            row.extend([""] * 5)

        data.append(row)

    return pd.DataFrame(data, columns=columns)


def main():
    # Setup
    event_engine = EventEngine()
    main_engine = MainEngine(event_engine)
    
    SYMBOL = "AAPL"
    CHAIN_DATE = "20251031"
    CHAIN_SYMBOL = f"{SYMBOL}_{CHAIN_DATE}"
    
    # Initialize portfolio and subscribe to chain
    portfolio = main_engine.get_portfolio(SYMBOL)
    main_engine.subscribe_chains("mock_test", [CHAIN_SYMBOL])
    main_engine.start_market_data_update()
    sleep(5)
    
    # Display option chain data
    for i in range(2):
        portfolio = main_engine.get_portfolio(SYMBOL)
        df = option_chain_iv_greeks_df(portfolio, CHAIN_SYMBOL)
        
        # Show only middle portion if too many rows
        if len(df) > 40:
            df = df.iloc[20:-20].reset_index(drop=True)
        
        underlying_price = portfolio.underlying.mid_price if portfolio.underlying else 0.0
        print(f"\nUnderlying ({SYMBOL}) Price: ${underlying_price:.2f}")
        print(f"Total options: {len(portfolio.options)}")
        print()
        
        with pd.option_context("display.width", None, "display.max_columns", None):
            print(df.to_string(index=False))
        print()
        sleep(5)
    
    # Cleanup
    main_engine.unsubscribe_chains("mock_test")
    main_engine.disconnect()
    main_engine.close()


if __name__ == "__main__":
    main()
