# Google Test 单元测试（与 non-framwork 脚本测试无关）
# 需 BUILD_TESTS=ON 且 find_package(GTest) 成功
set(OTRADER_UNIT_ROOT ${CMAKE_SOURCE_DIR})

set(OTRADER_GTEST_TARGETS)

function(otrader_add_gtest target src)
  add_executable(${target} ${src})
  target_include_directories(${target} PRIVATE ${OTRADER_UNIT_ROOT})
  target_link_libraries(${target} PRIVATE ${ARGN} GTest::gtest_main)
  set(OTRADER_GTEST_TARGETS ${OTRADER_GTEST_TARGETS} ${target} PARENT_SCOPE)
endfunction()

# Logical unit suites
otrader_add_gtest(test_system_data_flow_gtest test_system_data_flow.cpp utilities_cpp)
otrader_add_gtest(test_concurrency_design_gtest test_concurrency_design.cpp utilities_cpp)
otrader_add_gtest(test_concurrency_shutdown_gtest test_concurrency_shutdown.cpp utilities_cpp)

# §8.2 Ring buffer
otrader_add_gtest(test_spsc_ring_gtest test_spsc_ring.cpp utilities_cpp)
otrader_add_gtest(test_mpsc_ring_gtest test_mpsc_ring.cpp utilities_cpp)

# §8.3 Object pool
otrader_add_gtest(test_object_pool_gtest test_object_pool.cpp utilities_cpp)

# Core engines: execution + position (pure logic)
otrader_add_gtest(test_execution_engine_tracking_gtest test_execution_engine_tracking.cpp execution_engine_shared)
otrader_add_gtest(test_position_engine_invariants_gtest test_position_engine_invariants.cpp position_engine_shared)
otrader_add_gtest(test_position_serialize_roundtrip_gtest test_position_serialize_roundtrip.cpp position_engine_shared)

# Live-only
if(TARGET engines_cpp)
  otrader_add_gtest(test_zmq_message_flow_gtest test_zmq_message_flow.cpp engines_cpp)
  otrader_add_gtest(test_zmq_schema_negative_gtest test_zmq_schema_negative.cpp engines_cpp)
  otrader_add_gtest(test_intent_routing_gtest test_intent_routing.cpp engines_cpp)
  otrader_add_gtest(test_live_event_engine_safety_gtest test_live_event_engine_safety.cpp engines_cpp)
  otrader_add_gtest(test_live_shutdown_e2e_gtest test_live_shutdown_e2e.cpp engines_cpp)
  otrader_add_gtest(test_live_close_fast_gtest test_live_close_fast.cpp engines_cpp)
  # §8.1 Live dataflow（仅当 engines_cpp 存在时，即 BUILD_LIVE=ON）
  otrader_add_gtest(test_live_flow_gtest test_live_flow.cpp engines_cpp)
  otrader_add_gtest(test_live_event_engine_concurrency_mixed_gtest
                    test_live_event_engine_concurrency_mixed.cpp
                    engines_cpp)
  otrader_add_gtest(test_live_event_engine_drop_backpressure_gtest
                    test_live_event_engine_drop_backpressure.cpp
                    engines_cpp)
endif()

# Backtest-only
if(TARGET backtest_engines)
  otrader_add_gtest(test_backtest_event_dispatch_alignment_gtest
                    test_backtest_event_dispatch_alignment.cpp
                    backtest_engines)
  otrader_add_gtest(test_backtest_event_out_of_order_gtest
                    test_backtest_event_out_of_order.cpp
                    backtest_engines)
  otrader_add_gtest(test_backtest_event_duplicate_trade_gtest
                    test_backtest_event_duplicate_trade.cpp
                    backtest_engines)
  otrader_add_gtest(test_backtest_cancel_late_fill_gtest
                    test_backtest_cancel_late_fill.cpp
                    backtest_engines)
  otrader_add_gtest(test_backtest_terminal_status_regression_gtest
                    test_backtest_terminal_status_regression.cpp
                    backtest_engines)
  otrader_add_gtest(test_backtest_dispatch_order_contract_gtest
                    test_backtest_dispatch_order_contract.cpp
                    backtest_engines)
endif()

# 聚合 target：test_gtest.sh 一次构建所有 GTest 可执行文件
add_custom_target(otrader_gtest_all DEPENDS ${OTRADER_GTEST_TARGETS})

include(GoogleTest)
foreach(t IN LISTS OTRADER_GTEST_TARGETS)
  gtest_discover_tests(${t}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DISCOVERY_MODE PRE_TEST
  )
endforeach()
