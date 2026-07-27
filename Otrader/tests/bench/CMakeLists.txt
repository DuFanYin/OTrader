# Latency micro-benchmarks (L1) + throughput/IPC benchmarks (L2/L3). Built only when
# BUILD_BENCH=ON. MEASUREMENT executables, not correctness tests — kept out of CTest. Force
# -O3/NDEBUG so we measure Release behavior regardless of the configured build type.

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

# L2: replay throughput — needs the backtest engine (Arrow/Parquet). Only if that lib is built.
if(TARGET backtest_engines)
  add_executable(bench_replay bench_replay.cpp)
  target_include_directories(bench_replay PRIVATE
    ${BENCH_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../infra/marketdata
    ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/backtest
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
  )
  target_compile_options(bench_replay PRIVATE -O3 -DNDEBUG)
  target_link_libraries(bench_replay PRIVATE backtest_engines utilities_cpp)
  list(APPEND BENCH_TARGETS bench_replay)
else()
  message(STATUS "bench_replay skipped (needs BUILD_BACKTEST=ON for backtest_engines)")
endif()

# L3 (IPC latency / bench_ipc) removed: the ZMQ data plane it measured no longer exists — the
# live stack is a single process (market-data + gateway in-process; see runtime/live/engine_main).

add_custom_target(otrader_bench DEPENDS ${BENCH_TARGETS})
