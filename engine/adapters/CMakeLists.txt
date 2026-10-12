# The adapters to the outside: one folder per outside system, each a library of its own that
# declares its own dependencies, so a module sees only what it links and a third-party library
# stays in the module that uses it. Built for the runtimes this configuration builds:
# BUILD_BACKTEST, and live with any of BUILD_GATEWAY, BUILD_MARKETDATA, BUILD_LIVE_GRPC.
#   gateway (here): the venue port, adapters::Gateway; on its own, the venue with no broker.
#   ib/: IbGateway (gateway_core), IbFeed (ib_feed), the IB session (ib_common) and the TWS API.
#   marketdata/: the live quotes, whatever the feed (marketdata_live): the engine's market data
#     port, built with it (a backtest leaves the port empty).
#   backtest/: the backtest's venue (gateway_backtest) and its days from Parquet
#     (marketdata_backtest).
#   db/: the storage (db_sqlite), a file live and memory in a backtest.
if(BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  set(OPTRA_LIVE ON)
else()
  set(OPTRA_LIVE OFF)
endif()

if(OPTRA_LIVE OR BUILD_BACKTEST)
  add_library(gateway engine_gateway.cpp)
  target_include_directories(gateway PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(gateway PUBLIC model)
  add_subdirectory(db)
  add_subdirectory(marketdata)
endif()

if(BUILD_GATEWAY)
  add_subdirectory(ib)
elseif(OPTRA_LIVE)
  # No IB: the live build runs on the base Gateway (core::live::make_gateway).
  add_library(gateway_core INTERFACE)
  target_link_libraries(gateway_core INTERFACE gateway)
endif()
if(BUILD_BACKTEST)
  add_subdirectory(backtest)
endif()
