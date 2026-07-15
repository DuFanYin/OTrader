cmake_minimum_required(VERSION 3.16)
project(backtest_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Arrow can optionally look for lz4 CMake package and emit warnings
set(CMAKE_DISABLE_FIND_PACKAGE_lz4 TRUE)

# Depend on utilities and core (when built from Otrader root they are already added)
if(NOT TARGET utilities_cpp)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../utilities ${CMAKE_BINARY_DIR}/utilities)
endif()
if(NOT TARGET portfolio_structure_shared)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core ${CMAKE_BINARY_DIR}/core)
endif()

# Apache Arrow + Parquet (required for parquet loading)
find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
set(HAS_PARQUET ON)

# Compatibility aliases: some Arrow builds expose only *_shared/*_static targets.
if(NOT TARGET Arrow::arrow)
  if(TARGET Arrow::arrow_shared)
    add_library(Arrow::arrow INTERFACE IMPORTED)
    target_link_libraries(Arrow::arrow INTERFACE Arrow::arrow_shared)
  elseif(TARGET Arrow::arrow_static)
    add_library(Arrow::arrow INTERFACE IMPORTED)
    target_link_libraries(Arrow::arrow INTERFACE Arrow::arrow_static)
  else()
    message(FATAL_ERROR "Arrow target not found: Arrow::arrow / Arrow::arrow_shared / Arrow::arrow_static")
  endif()
endif()

if(NOT TARGET Parquet::parquet)
  if(TARGET Parquet::parquet_shared)
    add_library(Parquet::parquet INTERFACE IMPORTED)
    target_link_libraries(Parquet::parquet INTERFACE Parquet::parquet_shared)
  elseif(TARGET Parquet::parquet_static)
    add_library(Parquet::parquet INTERFACE IMPORTED)
    target_link_libraries(Parquet::parquet INTERFACE Parquet::parquet_static)
  else()
    message(FATAL_ERROR "Parquet target not found: Parquet::parquet / Parquet::parquet_shared / Parquet::parquet_static")
  endif()
endif()

add_library(backtest_engines
  engine_backtest.hpp
  engine_backtest.cpp
  engine_event.hpp
  engine_event.cpp
  engine_main.hpp
  engine_main.cpp
  ../main_engine_base.cpp
  ../../infra/gateway/engine_gateway_backtest.hpp
  ../../infra/gateway/engine_gateway_backtest.cpp
  ../../infra/marketdata/engine_data_historical.hpp
  ../../infra/marketdata/engine_data_historical.cpp
  ../../utilities/parquet_loader.hpp
  ../../utilities/parquet_loader.cpp
  ../../utilities/occ_utils.hpp
  ../../utilities/occ_utils.cpp
  ../../utilities/types.hpp
)

target_include_directories(backtest_engines PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../strategy
  ${CMAKE_CURRENT_SOURCE_DIR}/../../utilities
  ${CMAKE_CURRENT_SOURCE_DIR}/../../infra/gateway
  ${CMAKE_CURRENT_SOURCE_DIR}/../../infra/marketdata
)

target_link_libraries(backtest_engines
  PUBLIC
  utilities_cpp
  portfolio_structure_shared
  execution_engine_shared
  position_engine_shared
  hedge_engine_shared
  log_engine_shared
  strategies
  option_strategy_engine_shared
  Arrow::arrow_shared
  Parquet::parquet_shared
)
