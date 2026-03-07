import sqlite3
import sys
from datetime import UTC, datetime
from pathlib import Path

project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from utilities.constant import LOCAL_TZ
from utilities.utility import get_file_path


def analyze_options_by_expiry(symbol):
    """
    Analyze options by expiry date for a specific symbol

    Args:
        symbol: Symbol to analyze (e.g., 'AAPL', 'SPY')
    """
    # Connect to database
    db_path = Path(get_file_path("trading.db"))
    conn = sqlite3.connect(str(db_path))
    cursor = conn.cursor()

    try:

        # Query options for the specific symbol
        # Try multiple fields to find the symbol (symbol, underlying, portfolio)
        # Also try partial matches for more flexibility
        cursor.execute(
            """
            SELECT
                expiry,
                COUNT(*) as total_contracts,
                COUNT(DISTINCT strike) as strike_count,
                MIN(strike) as min_strike,
                MAX(strike) as max_strike
            FROM contract_option
            WHERE symbol = ? 
               OR underlying = ? 
               OR portfolio = ?
               OR symbol LIKE ? 
               OR underlying LIKE ?
               OR portfolio LIKE ?
            GROUP BY expiry
            ORDER BY expiry ASC
        """,
            (symbol, symbol, symbol, f"%{symbol}%", f"%{symbol}%", f"%{symbol}%"),
        )
        expiry_rows = cursor.fetchall()

        # Calculate days to expiry for each date using US/Eastern time
        now_utc = datetime.now(UTC)
        now_local = datetime.now()
        now_et = datetime.now(LOCAL_TZ)

        print("\nTime Check:")
        print(f"UTC Time     : {now_utc.strftime('%Y-%m-%d %H:%M:%S')} UTC")
        print(f"Local Time   : {now_local.strftime('%Y-%m-%d %H:%M:%S')} Local")
        print(f"Eastern Time : {now_et.strftime('%Y-%m-%d %H:%M:%S %Z')}")

        # Display header with symbol info
        print(f"\nOption Chain Analysis by Expiry for {symbol}:")
        print("=" * 80)
        print(
            f"{'Expiry Date':<12} {'DTE':<6} {'Contracts':<10} {'Strikes':<8} {'Strike Range':<20}"
        )
        print("-" * 80)

        today_et = datetime.now(UTC).date()

        total_options = 0
        for (
            expiry_date,
            contract_count,
            strike_count,
            min_strike,
            max_strike,
        ) in expiry_rows:
            # Parse expiry date as a date object
            expiry = datetime.strptime(expiry_date, "%Y-%m-%d").date()

            # Days-to-expiry using Eastern date as anchor
            dte = (expiry - today_et).days

            strike_range = f"{min_strike:.1f} - {max_strike:.1f}"
            print(
                f"{expiry_date:<12} {dte:<6} {contract_count:<10} {strike_count:<8} {strike_range:<20}"
            )

            total_options += contract_count

        print("-" * 80)
        print(f"Total Options: {total_options}")

    finally:
        conn.close()


if __name__ == "__main__":
    import sys

    symbol = "SPX"

    print(f"Analyzing options for symbol: {symbol}")
    analyze_options_by_expiry(symbol)
