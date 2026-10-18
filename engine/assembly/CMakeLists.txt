# The assemblies: Main with its backtest or live adapters. The only code that links adapters into it.
if(BUILD_BACKTEST)
  add_library(backtest backtest.cpp)
  target_link_libraries(backtest PUBLIC core backtest_gateway historical_data)
endif()

if(BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  # IB with BUILD_GATEWAY, else no broker and no quotes.
  add_library(live live.cpp)
  target_link_libraries(live PUBLIC core PRIVATE sqlite_database Threads::Threads)
  if(BUILD_GATEWAY)
    target_compile_definitions(live PRIVATE OPTRA_WITH_IB)
    target_link_libraries(live PRIVATE ib_gateway ib_market_data nlohmann_json::nlohmann_json)
  endif()
endif()
