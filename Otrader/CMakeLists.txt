cmake_minimum_required(VERSION 3.16)

project(Otrader LANGUAGES CXX)

# gRPC/protobuf headers use __has_feature (Clang builtin); GCC errors on them.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Otrader requires Clang; got '${CMAKE_CXX_COMPILER_ID}'.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clang-tidy and other tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -O3 for Release/RelWithDebInfo (Debug stays fast to build and debuggable)
add_compile_options(
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:RelWithDebInfo>:-O3>
)

# Protobuf must be CONFIG mode to match gRPC's find_dependency(Protobuf).
find_package(Protobuf CONFIG REQUIRED)
find_package(absl REQUIRED)
# gRPC is needed by otrader_proto (generated gRPC service stubs), which every configuration
# builds. find_dependency(Protobuf) inside gRPC's config is why Protobuf must be CONFIG mode too.
find_package(gRPC CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# cppzmq required for live engines (GatewayClient/MarketDataClient), Gateway, MarketData
if(BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  find_package(cppzmq CONFIG REQUIRED)
endif()

# 公共依赖：utilities、proto、core（backtest 与 live 共用）
add_subdirectory(utilities)
add_subdirectory(proto)
add_subdirectory(core)
add_subdirectory(strategy)

# 可选：回测入口 entry_backtest
option(BUILD_BACKTEST "Build backtest entry (entry_backtest)" OFF)
if(BUILD_BACKTEST)
  add_subdirectory(runtime/backtest)
  add_executable(entry_backtest entry_backtest.cpp)
  target_include_directories(entry_backtest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/backtest
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/strategy
  )
  target_link_libraries(entry_backtest PRIVATE backtest_engines)
endif()

# Gateway/MarketData/Live runtime libraries
option(BUILD_GATEWAY "Build Gateway components (used by entry_system)" OFF)
if(BUILD_GATEWAY)
  add_subdirectory(infra/gateway)
endif()

option(BUILD_MARKETDATA "Build Market Data components (used by entry_system)" OFF)
if(BUILD_MARKETDATA)
  if(NOT TARGET engines_cpp)
    add_subdirectory(runtime/live)
  endif()
endif()

option(BUILD_LIVE_GRPC "Build live runtime (engines_cpp) and entry_system" OFF)
if(BUILD_LIVE_GRPC)
  if(NOT TARGET engines_cpp)
    add_subdirectory(runtime/live)
  endif()
endif()

# 统一入口：entry_system（--mode=gateway|market|live|all 分发到 entry/ 下各独立 TU）。
# entry_system.cpp + run_live/run_market 不依赖任何 gateway 后端；只有 run_gateway.cpp 链 IB，
# 且仅在 BUILD_GATEWAY=ON 时编入。因此 live/market/backtest 可在没有 IBJts 的环境下构建。
if(BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  set(ENTRY_SOURCES
    entry_system.cpp
    entry/run_live.cpp
    entry/run_market.cpp)
  if(BUILD_GATEWAY)
    list(APPEND ENTRY_SOURCES entry/run_gateway.cpp)
  endif()
  add_executable(entry_system ${ENTRY_SOURCES})
  target_include_directories(entry_system PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/entry
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/live
    ${CMAKE_CURRENT_SOURCE_DIR}/utilities
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/infra/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/infra/marketdata
    ${CMAKE_CURRENT_SOURCE_DIR}/infra/db
  )
  if(TARGET engines_cpp)
    target_link_libraries(entry_system PRIVATE engines_cpp)
  endif()
  if(TARGET gateway_core)
    target_link_libraries(entry_system PRIVATE gateway_core)
    # run_gateway.cpp is the only entry TU that touches IB; gate it (and the gateway/all modes).
    target_compile_definitions(entry_system PRIVATE OTRADER_WITH_IB)
  endif()
  if(TARGET cppzmq)
    target_link_libraries(entry_system PRIVATE cppzmq)
  endif()
endif()

# 聚合 target：build.sh 只编此 target，一次构建所有主程序，避免多 target 顺序编时的重复进度输出
if(TARGET entry_backtest AND TARGET entry_system)
  add_custom_target(otrader_main
    DEPENDS entry_backtest entry_system)
endif()

# 测试（backtest 需 BUILD_BACKTEST；live 需 BUILD_LIVE_GRPC）
# 注意：BUILD_TESTS 现在**只**表示“启用 GTest/CTest 等测试基础设施”，
# 是否构建回测/实盘测试由 BUILD_BACKTEST / BUILD_LIVE 自己决定。
option(BUILD_TESTS "Build tests (GTest/CTest + optional script tests)" OFF)
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
# BUILD_LIVE 或 BUILD_BACKTEST 但未显式开启 BUILD_TESTS 时，如果仍然希望构建
# non-framwork 脚本测试，可按需在命令行同时加 -DBUILD_TESTS=ON。

# Latency micro-benchmarks (L1) — measurement executables, independent of BUILD_TESTS and not
# in CTest. Only need utilities_cpp (ring/pool are header-only). See local/benchmarkPlan.md.
option(BUILD_BENCH "Build latency micro-benchmarks (tests/bench)" OFF)
if(BUILD_BENCH)
  add_subdirectory(tests/bench)
endif()
