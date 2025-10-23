import sys
from pathlib import Path
from time import sleep

# Add project root to Python path for absolute imports
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from engines.engine_event import EventEngine
from engines.engine_main import MainEngine

# 1. Setup event engine and main engine
event_engine = EventEngine()
main_engine = MainEngine(event_engine)

# main_engine.connect()  # This will also load contracts # Wait for connection and contract loading

sleep(5)

contract = main_engine.get_contract("AAPL-20251024-C-290.0-100-USD-OPT")
if contract:
    fields = vars(contract)
    maxlen = max(len(str(field)) for field in fields)
    for field, value in fields.items():
        print(f"{field:<{maxlen}} : {value}")
else:
    print("Contract not found.")

main_engine.disconnect()
main_engine.close()
