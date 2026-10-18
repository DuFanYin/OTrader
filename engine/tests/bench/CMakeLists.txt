# Latency benchmarks, run by scripts/bench.sh, not by CTest; always -O3.

# The primitives.
function(add_bench name src)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_options(${name} PRIVATE -O3 -DNDEBUG)
  target_link_libraries(${name} PRIVATE model)
endfunction()

add_bench(bench_ring bench_ring.cpp)
add_bench(bench_object_pool bench_object_pool.cpp)
add_bench(bench_event_dispatch bench_event_dispatch.cpp)

set(BENCH_TARGETS bench_ring bench_object_pool bench_event_dispatch)

# The rest run the backtest engine.
if(TARGET backtest)
  function(add_engine_bench name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -O3 -DNDEBUG)
    target_link_libraries(${name} PRIVATE backtest model)
  endfunction()

  add_engine_bench(bench_replay bench_replay.cpp)
  add_engine_bench(bench_order_lifecycle bench_order_lifecycle.cpp)
  add_engine_bench(bench_backtest_day bench_backtest_day.cpp)
  optra_strategy_host(bench_backtest_day LOAD)
  # The live paths (a day of data/SPY, or OPTRA_TEST_DAY)
  add_engine_bench(bench_live_paths bench_live_paths.cpp)
  target_link_libraries(bench_live_paths PRIVATE sqlite_database) # its live assembly's storage
  target_compile_definitions(bench_live_paths PRIVATE
                             OPTRA_DATA_DIR="${CMAKE_SOURCE_DIR}/../data")
  list(APPEND BENCH_TARGETS bench_replay bench_order_lifecycle bench_backtest_day bench_live_paths)
else()
  message(STATUS "bench_replay/order_lifecycle/live_paths skipped (needs BUILD_BACKTEST=ON)")
endif()

# An order through the IB adapter to a TWS on a local port (a build with BUILD_GATEWAY).
if(TARGET ib_gateway)
  add_executable(bench_ib_order bench_ib_order.cpp)
  target_include_directories(bench_ib_order PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_options(bench_ib_order PRIVATE -O3 -DNDEBUG)
  target_link_libraries(bench_ib_order PRIVATE ib_gateway model)
  list(APPEND BENCH_TARGETS bench_ib_order)
endif()

# Compares the rows the benchmarks record (OPTRA_BENCH_ROWS) with baseline.tsv.
add_executable(bench_check bench_check.cpp)
target_compile_definitions(bench_check PRIVATE OPTRA_BENCH_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/baseline.tsv")
list(APPEND BENCH_TARGETS bench_check)

add_custom_target(optra_bench DEPENDS ${BENCH_TARGETS})
