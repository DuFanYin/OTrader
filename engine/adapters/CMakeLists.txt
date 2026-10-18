# Swappable implementations of core's ports, a folder per port; they depend on core, never back.
#   gateway/      the broker: IB (BUILD_GATEWAY), the backtest's venue, the simulated market's
#   market_data/  the quotes: IB's, the simulated market's; the market data of the past
#   database/     the storage: SQLite
#   ib_api/       IB's API as the engine uses it, which the IB gateway and market data are built on
if(BUILD_BACKTEST AND (BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC))
  set(OPTRA_SIMULATED_MARKET ON)
endif()
if(BUILD_GATEWAY)
  add_subdirectory(ib_api)
endif()
add_subdirectory(gateway)
add_subdirectory(market_data)
add_subdirectory(database)
