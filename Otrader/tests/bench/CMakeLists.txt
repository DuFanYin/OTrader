# Latency micro-benchmarks (L1) + throughput (L2) + order-lifecycle (L4) + HFT metrics (L5).
# Built only when BUILD_BENCH=ON. MEASUREMENT executables, not correctness tests — kept out of
# CTest. Force -O3/NDEBUG so we measure Release behavior regardless of the configured build type.
# (L3 IPC removed with the ZMQ data plane; see below.)

set(BENCH_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../utilities
)

# L1: pure engine primitives — only need utilities_cpp (ring/pool are header-only).
function(add_bench name src)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE ${BENCH_INCLUDES})
  target_compile_options(${name} PRIVATE -O3 -DNDEBUG)
  target_link_libraries(${name} PRIVATE utilities_cpp)
endfunction()

add_bench(bench_ring bench_ring.cpp)
add_bench(bench_object_pool bench_object_pool.cpp)
add_bench(bench_event_dispatch bench_event_dispatch.cpp)

set(BENCH_TARGETS bench_ring bench_object_pool bench_event_dispatch)

# L2/L4/L5: need the backtest engine (backtest MainEngine + core engines; L2 also Arrow/Parquet).
# Built only if that lib exists (BUILD_BACKTEST=ON). All are broker-free / DB-free.
if(TARGET backtest_engines)
  set(BENCH_ENGINE_INCLUDES
    ${BENCH_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../infra/marketdata
    ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/backtest
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
  )
  function(add_engine_bench name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE ${BENCH_ENGINE_INCLUDES})
    target_compile_options(${name} PRIVATE -O3 -DNDEBUG)
    target_link_libraries(${name} PRIVATE backtest_engines utilities_cpp)
  endfunction()

  add_engine_bench(bench_replay bench_replay.cpp)                    # L2 throughput
  add_engine_bench(bench_order_lifecycle bench_order_lifecycle.cpp)  # L4 order lifecycle
  add_engine_bench(bench_hft_metrics bench_hft_metrics.cpp)          # L5 HFT metrics
  list(APPEND BENCH_TARGETS bench_replay bench_order_lifecycle bench_hft_metrics)
else()
  message(STATUS "bench_replay/order_lifecycle/hft_metrics skipped (needs BUILD_BACKTEST=ON)")
endif()

# L3 (IPC latency / bench_ipc) removed: the ZMQ data plane it measured no longer exists — the
# live stack is a single process (market-data + gateway in-process; see runtime/live/engine_main).

add_custom_target(otrader_bench DEPENDS ${BENCH_TARGETS})
