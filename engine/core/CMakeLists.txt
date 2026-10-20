# core: one library per engine, and Main; the ports are declared here and implemented in adapters/.
# The assemblies (assembly/) put Main together with its adapters.
find_package(Threads REQUIRED)

# The ports. MarketData is declared with MarketDataEngine, which its adapters write quotes into.
add_library(gateway gateway/gateway.cpp)
target_link_libraries(gateway PUBLIC model)

add_library(database INTERFACE)
target_link_libraries(database INTERFACE model)

# State-only engines: no I/O.
add_library(bookkeeping_engine bookkeeping/bookkeeping_engine.cpp)
target_link_libraries(bookkeeping_engine PUBLIC model)

add_library(risk_engine risk/risk_engine.cpp)
target_link_libraries(risk_engine PUBLIC bookkeeping_engine)

add_library(strategy_engine strategy/strategy_engine.cpp)
target_link_libraries(strategy_engine PUBLIC bookkeeping_engine strategies)

# I/O engines, each on its own thread.
add_library(log_engine log/log_engine.cpp)
target_link_libraries(log_engine PUBLIC model PRIVATE Threads::Threads)

# Plans what to stream from what the strategies want (StrategyEngine::wanted), prices with pricing/.
add_library(market_data_engine market_data/market_data_engine.cpp)
target_link_libraries(market_data_engine PUBLIC strategy_engine PRIVATE pricing Threads::Threads)

# The event thread: hands events to the handler it is given (Main::process_event).
add_library(event_loop event_loop.cpp)
target_link_libraries(event_loop PUBLIC model PRIVATE Threads::Threads)

add_library(core main.cpp)
target_link_libraries(core PUBLIC
  event_loop bookkeeping_engine risk_engine strategy_engine
  log_engine market_data_engine database gateway)
