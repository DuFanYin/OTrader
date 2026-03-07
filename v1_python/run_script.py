from time import sleep

from engines.engine_event import EventEngine
from engines.engine_main import MainEngine

if __name__ == "__main__":

    # Setup engines
    event_engine = EventEngine()
    main_engine = MainEngine(event_engine)


    STRATEGY_CLASS_NAME = "Test"
    PORTFOLIO_NAME = "AAPL"
    SETTING = {}

    DURATION = 120

    try:
        # Connect and wait for stable connection
        main_engine.connect()

        # Start market data update
        main_engine.start_market_data_update()
        sleep(5)

        # Add and start strategy
        strategy_engine = main_engine.option_strategy_engine
        strategy_engine.add_strategy(class_name=STRATEGY_CLASS_NAME, portfolio_name=PORTFOLIO_NAME, setting=SETTING)

        strategy_name = f"{STRATEGY_CLASS_NAME}_{PORTFOLIO_NAME}"
        strategy_engine.init_strategy(strategy_name)
        sleep(3)

        strategy_engine.start_strategy(strategy_name)
        sleep(DURATION)

        # Clean shutdown
        strategy_engine.stop_strategy(strategy_name)
        sleep(5)

    finally:
        # Disconnect gateway and close engines
        main_engine.disconnect()
        main_engine.close()
