# The GTest suites (the checks in tests/checks are not among them); BUILD_TESTS=ON and GTest found
set(OPTRA_UNIT_ROOT ${CMAKE_SOURCE_DIR})

set(OPTRA_GTEST_TARGETS)

function(optra_add_gtest target src)
  add_executable(${target} ${src})
  target_include_directories(${target} PRIVATE ${OPTRA_UNIT_ROOT})
  target_link_libraries(${target} PRIVATE ${ARGN} GTest::gtest_main)
  set(OPTRA_GTEST_TARGETS ${OPTRA_GTEST_TARGETS} ${target} PARENT_SCOPE)
endfunction()

# Logical unit suites
optra_add_gtest(test_system_data_flow_gtest test_system_data_flow.cpp model)
optra_add_gtest(test_concurrency_design_gtest test_concurrency_design.cpp model)
optra_add_gtest(test_concurrency_shutdown_gtest test_concurrency_shutdown.cpp model)

# Ring buffers
optra_add_gtest(test_mpsc_ring_gtest test_mpsc_ring.cpp model)

# Object pool
optra_add_gtest(test_object_pool_gtest test_object_pool.cpp model)

# Instrument names (utilities/symbol)
optra_add_gtest(test_symbol_gtest test_symbol.cpp model)

# The strategy layer alone (no engine linked in)
optra_add_gtest(test_strategy_context_gtest test_strategy_context.cpp strategies)

# The engines core holds, each on its own (built with core: a backtest or live build)
if(TARGET core)
  optra_add_gtest(test_execution_engine_tracking_gtest test_execution_engine_tracking.cpp core)
  optra_add_gtest(test_position_engine_invariants_gtest test_position_engine_invariants.cpp core)
  optra_add_gtest(test_log_engine_gtest test_log_engine.cpp core)
  optra_add_gtest(test_hedge_engine_gtest test_hedge_engine.cpp core)
  optra_add_gtest(test_settlement_gtest test_settlement.cpp core)
  optra_add_gtest(test_risk_engine_gtest test_risk_engine.cpp core)
endif()

# Live-only
if(TARGET core_live)
  optra_add_gtest(test_live_event_engine_safety_gtest test_live_event_engine_safety.cpp core_live)
  optra_add_gtest(test_live_shutdown_e2e_gtest test_live_shutdown_e2e.cpp core_live)
  # Live dataflow (a live build: core_live exists)
  optra_add_gtest(test_live_flow_gtest test_live_flow.cpp core_live)
  # A restart resumes: strategies, holdings, settlement (a strategy class from the factory)
  find_package(SQLite3 REQUIRED)  # these tests also change the file by hand
  optra_add_gtest(test_live_restore_gtest test_live_restore.cpp core_live strategy_factory
                    SQLite::SQLite3)
  optra_add_gtest(test_database_engine_gtest test_database_engine.cpp db_sqlite SQLite::SQLite3)
  optra_add_gtest(test_market_data_engine_gtest test_market_data_engine.cpp core_live)
  optra_add_gtest(test_live_event_engine_concurrency_mixed_gtest
                    test_live_event_engine_concurrency_mixed.cpp
                    core_live)
  optra_add_gtest(test_live_event_engine_burst_gtest
                    test_live_event_engine_burst.cpp
                    core_live)
  # Connect, catch up, check the positions, then start: the live engine over a stand-in broker
  optra_add_gtest(test_live_connect_gtest test_live_connect.cpp core_live)
endif()

# IB gateway build only
if(TARGET ib_common)
  optra_add_gtest(test_ib_contracts_gtest test_ib_contracts.cpp ib_common)
  optra_add_gtest(test_ib_gateway_gtest test_ib_gateway.cpp gateway_core ib_common)
endif()

# Backtest-only
if(TARGET core_backtest)
  optra_add_gtest(test_backtest_event_dispatch_alignment_gtest
                    test_backtest_event_dispatch_alignment.cpp
                    core_backtest)
  optra_add_gtest(test_backtest_event_out_of_order_gtest
                    test_backtest_event_out_of_order.cpp
                    core_backtest)
  optra_add_gtest(test_backtest_event_duplicate_trade_gtest
                    test_backtest_event_duplicate_trade.cpp
                    core_backtest)
  optra_add_gtest(test_backtest_cancel_late_fill_gtest
                    test_backtest_cancel_late_fill.cpp
                    core_backtest)
  optra_add_gtest(test_backtest_terminal_status_regression_gtest
                    test_backtest_terminal_status_regression.cpp
                    core_backtest)
  optra_add_gtest(test_backtest_dispatch_order_contract_gtest
                    test_backtest_dispatch_order_contract.cpp
                    core_backtest)
  optra_add_gtest(test_backtest_combo_leg_prices_gtest
                    test_backtest_combo_leg_prices.cpp
                    core_backtest)
  optra_add_gtest(test_occ_expiry_gtest test_occ_expiry.cpp marketdata_backtest)
  optra_add_gtest(test_strategy_bar_clock_gtest test_strategy_bar_clock.cpp core_backtest)
  optra_add_gtest(test_strategy_settings_gtest test_strategy_settings.cpp core_backtest)
  optra_add_gtest(test_parquet_loader_gtest test_parquet_loader.cpp marketdata_backtest)
  # The live event loop over the engine: never drops, keeps order, the newest snapshot, its own
  # timer; a day through it trades as the backtest does (data/SPY/SPY-2025-03/20250303.parquet,
  # or OPTRA_TEST_DAY; skipped without one)
  optra_add_gtest(test_live_event_loop_gtest test_live_event_loop.cpp core_backtest
                    strategy_factory)
  target_compile_definitions(test_live_event_loop_gtest PRIVATE
                             OPTRA_DATA_DIR="${CMAKE_SOURCE_DIR}/../data")
endif()

# Every GTest executable in one target: cmake --build engine/build --target optra_gtest_all
add_custom_target(optra_gtest_all DEPENDS ${OPTRA_GTEST_TARGETS})

include(GoogleTest)
foreach(t IN LISTS OPTRA_GTEST_TARGETS)
  gtest_discover_tests(${t}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DISCOVERY_MODE PRE_TEST
  )
endforeach()
