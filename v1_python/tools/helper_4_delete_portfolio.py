import sys
import traceback
from datetime import datetime
from pathlib import Path
from time import sleep

# Add project root to Python path for absolute imports
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from engines.engine_event import Event, EventEngine
from engines.engine_main import MainEngine
from utilities.event import EVENT_LOG, EVENT_OPTION_PORTFOLIO_COMPLETE

# 1. Setup event engine and main engine
event_engine = EventEngine()
main_engine = MainEngine(event_engine)

# Global flags to track option portfolio progress
option_portfolio_complete = False
progress_updates = []


def on_option_portfolio_complete(event: Event):
    global option_portfolio_complete, completion_data, contract_count
    completion_data = event.data
    option_portfolio_complete = True


main_engine.connect()


try:
    # Wait for connection to establish
    sleep(3)

    main_engine.db_engine.delete_portfolio("SPX")

except Exception as e:
    main_engine.write_log(f"Error occurred: {str(e)}")
    main_engine.write_log(f"Traceback: {traceback.format_exc()}")

finally:
    # Proper cleanup
    main_engine.disconnect()
    main_engine.close()
