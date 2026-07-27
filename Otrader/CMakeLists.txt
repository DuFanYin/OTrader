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

# Live runtime. The engine is a single process (market-data + gateway are in-process inside
# MainEngine), so BUILD_MARKETDATA/BUILD_LIVE_GRPC now select the same build; kept as separate
# options for compatibility. gateway_core is always built for a live build; BUILD_GATEWAY only
# chooses IbGateway (IB) vs NullGateway (no-IB) inside infra/gateway (see live_gateway.hpp).
option(BUILD_GATEWAY "Use the real Interactive Brokers gateway (IbGateway); off = NullGateway" OFF)
option(BUILD_MARKETDATA "Build the live runtime (engines_cpp)" OFF)
option(BUILD_LIVE_GRPC "Build the live runtime (engines_cpp) and entry_system" OFF)

if(BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  add_subdirectory(infra/gateway)     # defines gateway_core (IbGateway or NullGateway)
  add_subdirectory(runtime/live)      # defines engines_cpp (links gateway_core)

  add_executable(entry_system
    entry_system.cpp
    entry/run_live.cpp)
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
  target_link_libraries(entry_system PRIVATE engines_cpp gateway_core)
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
