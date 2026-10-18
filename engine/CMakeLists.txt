cmake_minimum_required(VERSION 3.16)

project(Optra LANGUAGES CXX)

# GCC 14 or newer: C++23, std::chrono time zones, and what the gRPC/protobuf headers use.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR
    "Optra builds with GCC 14 or newer; got ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# compile_commands.json: clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:RelWithDebInfo>:-O3>
)

# Ubuntu ships no protobuf CMake config: FindProtobuf, as gRPC's own config does.
find_package(Protobuf REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

option(BUILD_BACKTEST "Build backtest entry (entry_backtest)" OFF)
option(BUILD_GATEWAY "Use the real Interactive Brokers gateway (IbGateway); off = no broker" OFF)
option(BUILD_MARKETDATA "Build the live runtime (live)" OFF)
option(BUILD_LIVE_GRPC "Build the live runtime (live) and entry_live" OFF)
option(BUILD_ANALYTICS "Build the analytics' C library for the Python server (libanalytics_c.so)" OFF)

include(cmake/CodeIdentity.cmake)

add_subdirectory(quant)
add_subdirectory(utilities)
add_subdirectory(model)
add_subdirectory(strategy)

if(BUILD_BACKTEST OR BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  add_subdirectory(core)
  add_subdirectory(adapters)
  add_subdirectory(assembly)
  add_subdirectory(entry)
endif()

# What build.sh builds: the programs, the strategies, the analytics library.
if(TARGET entry_backtest AND TARGET entry_live)
  add_custom_target(optra_main DEPENDS entry_backtest entry_live optra_strategies)
  if(TARGET entry_simulate)
    add_dependencies(optra_main entry_simulate)
  endif()
  if(TARGET analytics_c)
    add_dependencies(optra_main analytics_c)
  endif()
endif()

option(BUILD_TESTS "Build tests (GTest/CTest + optional script tests)" OFF)
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  # The server's tests run these checks from this build: kept current with the strategies.
  if(TARGET optra_main AND TARGET test_backtest_data)
    add_dependencies(optra_main test_backtest_data test_hedge_backtest)
  endif()
endif()

# Latency benchmarks, not in CTest: scripts/bench.sh runs them.
option(BUILD_BENCH "Build latency micro-benchmarks (tests/bench)" OFF)
if(BUILD_BENCH)
  add_subdirectory(tests/bench)
endif()
