cmake_minimum_required(VERSION 3.16)

# gRPC/protobuf headers use __has_feature (Clang-only). GCC triggers "missing '(' after '__has_feature'".
# Prefer Clang when user hasn't explicitly set compiler.
if(NOT DEFINED CMAKE_CXX_COMPILER OR CMAKE_CXX_COMPILER STREQUAL "")
  find_program(CLANGXX NAMES clang++ HINTS /opt/homebrew/opt/llvm/bin)
  if(CLANGXX)
    set(CMAKE_CXX_COMPILER "${CLANGXX}" CACHE STRING "C++ compiler" FORCE)
  endif()
endif()

project(Otrader LANGUAGES CXX)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR
    "Otrader requires Clang (gRPC/protobuf use __has_feature).\n"
    "  Use: cmake -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ ...\n"
    "  Or:  ./build.sh")
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

# 依赖统一用 Homebrew（build.sh 已设 CMAKE_PREFIX_PATH=/opt/homebrew）
# 必须用 CONFIG 模式，与 gRPC 的 find_dependency(Protobuf) 一致，避免 target 重复定义
find_package(Protobuf CONFIG REQUIRED)
find_package(absl REQUIRED)
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

# 统一入口：entry_system（根据 --mode=gateway|market|live|all 分发）
if(BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  add_executable(entry_system entry_system.cpp)
  target_include_directories(entry_system PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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
