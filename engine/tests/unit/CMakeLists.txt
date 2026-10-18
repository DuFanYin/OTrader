# The GTest suites, in folders that mirror engine/: a test sits where the code it tests does.
set(OPTRA_UNIT_ROOT ${CMAKE_SOURCE_DIR})

set(OPTRA_GTEST_TARGETS)

function(optra_add_gtest target src)
  add_executable(${target} ${src})
  target_include_directories(${target} PRIVATE ${OPTRA_UNIT_ROOT})
  target_link_libraries(${target} PRIVATE ${ARGN} GTest::gtest_main)
  set(OPTRA_GTEST_TARGETS ${OPTRA_GTEST_TARGETS} ${target} PARENT_SCOPE)
endfunction()

# utilities/
optra_add_gtest(test_mpsc_queue_gtest utilities/test_mpsc_queue.cpp model)
optra_add_gtest(test_thread_pool_gtest utilities/test_thread_pool.cpp model)
optra_add_gtest(test_mpsc_ring_gtest utilities/test_mpsc_ring.cpp model)
optra_add_gtest(test_object_pool_gtest utilities/test_object_pool.cpp model)
optra_add_gtest(test_wakeup_gtest utilities/test_wakeup.cpp model)
optra_add_gtest(test_latency_gtest utilities/test_latency.cpp model)

# model/
optra_add_gtest(test_symbol_gtest model/test_symbol.cpp model)
optra_add_gtest(test_daily_table_gtest model/test_daily_table.cpp model)
optra_add_gtest(test_occ_expiry_gtest model/test_occ_expiry.cpp model)
optra_add_gtest(test_object_gtest model/test_object.cpp model)
optra_add_gtest(test_order_builder_gtest model/test_order_builder.cpp model)
optra_add_gtest(test_portfolio_gtest model/test_portfolio.cpp model)
optra_add_gtest(test_parse_date_gtest model/test_parse_date.cpp model)
optra_add_gtest(test_constant_gtest model/test_constant.cpp model)

# strategy/
optra_add_gtest(test_strategy_context_gtest strategy/test_strategy_context.cpp strategies)

# core/: the engines, each alone
if(TARGET core)
  optra_add_gtest(test_bookkeeping_orders_gtest core/bookkeeping/test_bookkeeping_orders.cpp bookkeeping_engine)
  optra_add_gtest(test_bookkeeping_holdings_gtest core/bookkeeping/test_bookkeeping_holdings.cpp bookkeeping_engine)
  optra_add_gtest(test_settlement_gtest core/bookkeeping/test_settlement.cpp core)
  optra_add_gtest(test_risk_engine_gtest core/risk/test_risk_engine.cpp risk_engine)
  optra_add_gtest(test_strategy_hedging_gtest core/strategy/test_strategy_hedging.cpp
                  strategy_engine)
  optra_add_gtest(test_log_engine_gtest core/log/test_log_engine.cpp log_engine)
endif()

# core/: the live assembly, its event loop and market data
if(TARGET live)
  optra_add_gtest(test_event_loop_safety_gtest core/event_loop/test_event_loop_safety.cpp
                  event_loop)
  optra_add_gtest(test_event_loop_concurrency_mixed_gtest
                  core/event_loop/test_event_loop_concurrency_mixed.cpp event_loop)
  optra_add_gtest(test_event_loop_timing_gtest core/event_loop/test_event_loop_timing.cpp
                  event_loop)
  optra_add_gtest(test_event_loop_burst_gtest core/event_loop/test_event_loop_burst.cpp
                  event_loop)
  optra_add_gtest(test_live_shutdown_e2e_gtest core/event_loop/test_live_shutdown_e2e.cpp
                  event_loop)
  optra_add_gtest(test_live_flow_gtest core/live/test_live_flow.cpp live)
  optra_add_gtest(test_live_connect_gtest core/live/test_live_connect.cpp live)
  find_package(SQLite3 REQUIRED)
  optra_add_gtest(test_live_restore_gtest core/live/test_live_restore.cpp live
                  sqlite_database SQLite::SQLite3)
  optra_strategy_host(test_live_restore_gtest LOAD)
  optra_add_gtest(test_market_data_engine_gtest core/market_data/test_market_data_engine.cpp
                  live sqlite_database)
endif()

# core/: Main's booking, the strategies and the event loop, over the backtest assembly; the runs
# over many days
if(TARGET backtest)
  optra_add_gtest(test_booking_order_and_trade_gtest
                  core/main/test_booking_order_and_trade.cpp backtest)
  optra_add_gtest(test_booking_out_of_order_gtest
                  core/main/test_booking_out_of_order.cpp backtest)
  optra_add_gtest(test_booking_duplicate_trade_gtest
                  core/main/test_booking_duplicate_trade.cpp backtest)
  optra_add_gtest(test_booking_cancel_late_fill_gtest
                  core/main/test_booking_cancel_late_fill.cpp backtest)
  optra_add_gtest(test_booking_terminal_status_gtest
                  core/main/test_booking_terminal_status.cpp backtest)
  optra_add_gtest(test_booking_before_strategy_callbacks_gtest
                  core/main/test_booking_before_strategy_callbacks.cpp backtest)
  optra_add_gtest(test_main_operations_gtest core/main/test_main_operations.cpp backtest)
  optra_add_gtest(test_strategy_bar_clock_gtest core/strategy/test_strategy_bar_clock.cpp
                  backtest)
  optra_add_gtest(test_strategy_settings_gtest core/strategy/test_strategy_settings.cpp
                  backtest)
  optra_add_gtest(test_strategy_engine_gtest core/strategy/test_strategy_engine.cpp
                  backtest)
  optra_add_gtest(test_template_orders_gtest strategy/test_template_orders.cpp backtest)
  optra_add_gtest(test_live_event_loop_gtest core/event_loop/test_live_event_loop.cpp
                  backtest)
  optra_strategy_host(test_live_event_loop_gtest LOAD)
  target_compile_definitions(test_live_event_loop_gtest PRIVATE
                             OPTRA_DATA_DIR="${CMAKE_SOURCE_DIR}/../data")
  optra_add_gtest(test_backtest_multiday_gtest core/backtest/test_backtest_multiday.cpp
                  backtest Arrow::arrow Parquet::parquet)
  optra_strategy_host(test_backtest_multiday_gtest LOAD)
  optra_add_gtest(test_strategy_libraries_gtest strategy/test_strategy_libraries.cpp model)
  optra_strategy_host(test_strategy_libraries_gtest)
endif()

# adapters/
if(TARGET core)
  find_package(SQLite3 REQUIRED)
  optra_add_gtest(test_sqlite_database_gtest adapters/database/test_sqlite_database.cpp
                  sqlite_database SQLite::SQLite3)
endif()
if(TARGET ib_api)
  optra_add_gtest(test_ib_contracts_gtest adapters/ib_api/test_ib_contracts.cpp ib_api)
  optra_add_gtest(test_ib_gateway_gtest adapters/gateway/test_ib_gateway.cpp ib_gateway)
endif()
if(TARGET backtest)
  optra_add_gtest(test_backtest_gateway_fill_model_gtest
                  adapters/gateway/test_backtest_gateway_fill_model.cpp backtest)
  optra_add_gtest(test_backtest_gateway_combo_legs_gtest
                  adapters/gateway/test_backtest_gateway_combo_legs.cpp backtest)
  optra_add_gtest(test_parquet_quotes_gtest adapters/market_data/test_parquet_quotes.cpp
                  historical_data Arrow::arrow Parquet::parquet)
  if(TARGET simulated_market_data)
    target_link_libraries(test_parquet_quotes_gtest PRIVATE simulated_market_data)
  endif()
endif()
if(TARGET simulated_gateway)
  optra_add_gtest(test_simulated_gateway_gtest adapters/gateway/test_simulated_gateway.cpp
                  simulated_gateway)
endif()

# All of them: cmake --build engine/build --target optra_gtest_all
add_custom_target(optra_gtest_all DEPENDS ${OPTRA_GTEST_TARGETS})

include(GoogleTest)
foreach(t IN LISTS OPTRA_GTEST_TARGETS)
  gtest_discover_tests(${t}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DISCOVERY_MODE PRE_TEST
  )
endforeach()
