import sys
from pathlib import Path
from time import sleep

# Add project root to Python path for absolute imports
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from engines import EventEngine, MainEngine

def check_main_engine() -> None:
    """Simple system check - MainEngine will raise errors if anything fails during init"""
    try:
        event_engine = EventEngine()
        main_engine = MainEngine(event_engine)
        main_engine.connect()
        sleep(3)

        main_engine.close()

    except Exception as e:
        # If MainEngine fails to init, it will raise an exception
        print(f"❌ System check failed: {str(e)}")
        import traceback

        print(f"Traceback: {traceback.format_exc()}")
        return


if __name__ == "__main__":
    check_main_engine()
