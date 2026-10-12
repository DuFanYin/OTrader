cmake_minimum_required(VERSION 3.16)

project(Optra VERSION 0.2.0 LANGUAGES CXX)

# GCC, like the Ubuntu packages it links against; 14 or newer for C++23, the std::chrono time
# zones (utilities::option_expiry_time) and the __has_feature the gRPC/protobuf headers use.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR
    "Optra builds with GCC 14 or newer; got ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clang-tidy and other tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -O3 for Release/RelWithDebInfo (Debug stays fast to build and debuggable)
add_compile_options(
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:RelWithDebInfo>:-O3>
)

# Protobuf is found the way gRPC's own config finds it: Ubuntu ships no protobuf CMake config,
# so both go through CMake's FindProtobuf module (protobuf::libprotobuf).
find_package(Protobuf REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# What to build: the backtest entry (entry_backtest), the live runtime below, the analytics
option(BUILD_BACKTEST "Build backtest entry (entry_backtest)" OFF)
option(BUILD_GATEWAY "Use the real Interactive Brokers gateway (IbGateway); off = no broker" OFF)
option(BUILD_MARKETDATA "Build the live runtime (core_live)" OFF)
option(BUILD_LIVE_GRPC "Build the live runtime (core_live) and entry_live" OFF)
option(BUILD_ANALYTICS "Build the analytics' C library for the Python server (libanalytics_c.so)" OFF)

# Shared by backtest and live: quant, utilities, model, strategy, adapters
add_subdirectory(quant)     # the numerics (OpenMP); with BUILD_ANALYTICS, the research tools' C library
add_subdirectory(utilities)
add_subdirectory(model)
add_subdirectory(strategy)
add_subdirectory(adapters) # one folder per outside system, each its own library

if(BUILD_BACKTEST OR BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  add_subdirectory(core)  # the engine, its engines, and its backtest and live assemblies
  add_subdirectory(entry) # the executables, and the live one's gRPC service (the control plane's way in)
endif()

# The one target build.sh builds: the executables (and the analytics library), in one go
if(TARGET entry_backtest AND TARGET entry_live)
  add_custom_target(optra_main DEPENDS entry_backtest entry_live)
  if(TARGET entry_simulate)
    add_dependencies(optra_main entry_simulate)
  endif()
  if(TARGET analytics_c)
    add_dependencies(optra_main analytics_c)
  endif()
endif()

# Tests. BUILD_TESTS only turns on GTest/CTest; which suites build follows BUILD_BACKTEST and the
# live options.
option(BUILD_TESTS "Build tests (GTest/CTest + optional script tests)" OFF)
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Latency benchmarks — measurement executables, independent of BUILD_TESTS and not in CTest; the
# primitives need only model and utilities_cpp (ring/pool are header-only). scripts/bench.sh runs
# them.
option(BUILD_BENCH "Build latency micro-benchmarks (tests/bench)" OFF)
if(BUILD_BENCH)
  add_subdirectory(tests/bench)
endif()
