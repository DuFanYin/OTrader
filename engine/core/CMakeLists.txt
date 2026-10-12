# core: the engine (core::Engine, engine_main) and its event loop (engine_event), with the
# engines it holds (engines/: execution, positions, hedging, risk, strategies, the log); the
# ports it reaches the world through are adapters/. Its two assemblies are built by their own
# option, so a backtest build pulls in no IB and no gRPC:
#   core_backtest  core::backtest::MainEngine and its run over one day or many (BUILD_BACKTEST)
#   core_live      core::live::MainEngine: IB with BUILD_GATEWAY, else no broker
add_library(core
  engines/execution.cpp
  engines/position.cpp
  engines/hedge.cpp
  engines/risk.cpp
  engines/option_strategy.cpp
  engines/log.cpp
  engine_main.cpp
  engine_event.cpp
)
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(core PUBLIC model strategies gateway db_sqlite marketdata_live)

if(BUILD_BACKTEST)
  add_library(core_backtest engine_backtest.cpp)
  target_link_libraries(core_backtest PUBLIC core gateway_backtest marketdata_backtest)
endif()

if(BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  find_package(Threads REQUIRED)
  add_library(core_live engine_live.cpp)
  target_link_libraries(core_live PUBLIC core gateway_core Threads::Threads)
  if(BUILD_GATEWAY)
    target_link_libraries(core_live PUBLIC ib_feed)
  endif()
endif()
