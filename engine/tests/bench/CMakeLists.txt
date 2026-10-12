# Latency benchmarks: the primitives, replay throughput, the order lifecycle, the live paths.
# Built only when BUILD_BENCH=ON (scripts/bench.sh). MEASUREMENT executables, not correctness tests
# — kept out of CTest. Force -O3/NDEBUG so we measure Release behavior regardless of the configured
# build type.

set(BENCH_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../utilities
)

# The primitives — only need model (and utilities_cpp; ring/pool are header-only).
function(add_bench name src)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE ${BENCH_INCLUDES})
  target_compile_options(${name} PRIVATE -O3 -DNDEBUG)
  target_link_libraries(${name} PRIVATE model)
endfunction()

add_bench(bench_ring bench_ring.cpp)
add_bench(bench_object_pool bench_object_pool.cpp)
add_bench(bench_event_dispatch bench_event_dispatch.cpp)

set(BENCH_TARGETS bench_ring bench_object_pool bench_event_dispatch)

# The rest need the backtest engine (backtest MainEngine + core engines; replay also Arrow/Parquet).
# Built only if that lib exists (BUILD_BACKTEST=ON). All are broker-free / DB-free.
if(TARGET core_backtest)
  set(BENCH_ENGINE_INCLUDES
    ${BENCH_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../adapters/backtest
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
  )
  function(add_engine_bench name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE ${BENCH_ENGINE_INCLUDES})
    target_compile_options(${name} PRIVATE -O3 -DNDEBUG)
    target_link_libraries(${name} PRIVATE core_backtest model)
  endfunction()

  add_engine_bench(bench_replay bench_replay.cpp)                    # replay throughput
  add_engine_bench(bench_order_lifecycle bench_order_lifecycle.cpp)  # order lifecycle
  # The live paths through the event thread (a day of data/SPY, or OPTRA_TEST_DAY)
  add_engine_bench(bench_live_paths bench_live_paths.cpp)
  target_compile_definitions(bench_live_paths PRIVATE
                             OPTRA_DATA_DIR="${CMAKE_SOURCE_DIR}/../data")
  list(APPEND BENCH_TARGETS bench_replay bench_order_lifecycle bench_live_paths)
else()
  message(STATUS "bench_replay/order_lifecycle/live_paths skipped (needs BUILD_BACKTEST=ON)")
endif()

add_custom_target(optra_bench DEPENDS ${BENCH_TARGETS})
