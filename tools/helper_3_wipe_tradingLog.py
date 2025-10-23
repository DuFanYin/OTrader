#!/usr/bin/env python3
"""
Helper script to wipe trading data (orders and trades) from the database.
This script preserves contract data but removes all order and trade history.

Usage:
    python helper_1_wipe_tradingLog.py
"""

import sys
from pathlib import Path

# Ensure project root is on sys.path
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from engines.engine_event import EventEngine
from engines.engine_main import MainEngine


def main() -> None:
    """Main function to wipe trading data."""
    try:
        # Initialize engines
        event_engine = EventEngine()
        main_engine = MainEngine(event_engine)
        
        # Perform the wipe
        main_engine.wipe_trading_data()
        
        # Close engines
        main_engine.close()
        
    except Exception as e:
        sys.exit(1)


if __name__ == "__main__":
    main()
