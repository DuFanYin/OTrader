cmake_minimum_required(VERSION 3.16)
project(engines_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(gRPC REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Depend on utilities and core (Otrader layout)
if(NOT TARGET utilities_cpp)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../utilities ${CMAKE_BINARY_DIR}/utilities)
endif()
if(NOT TARGET portfolio_structure_shared)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core ${CMAKE_BINARY_DIR}/core)
endif()

add_library(engines_cpp
  engine_event.cpp
  gateway_client.cpp
  market_data_client.cpp
  ../main_engine_base.cpp
  ../../infra/db/engine_db_pg.cpp
  ../../infra/gateway/zmq_gateway_schema.cpp
  ../../infra/marketdata/engine_data_tradier.cpp
  ../../infra/marketdata/zmq_marketdata_schema.cpp
  engine_main.cpp
  engine_grpc.cpp
)
target_include_directories(engines_cpp PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core
  ${CMAKE_CURRENT_SOURCE_DIR}/../../utilities
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../proto
  ${CMAKE_CURRENT_SOURCE_DIR}/../../strategy
  ${CMAKE_CURRENT_SOURCE_DIR}/../../infra/db
  ${CMAKE_CURRENT_SOURCE_DIR}/../../infra/gateway
  ${CMAKE_CURRENT_SOURCE_DIR}/../../infra/marketdata
)

target_link_libraries(engines_cpp PRIVATE Threads::Threads CURL::libcurl nlohmann_json::nlohmann_json cppzmq)

# PostgreSQL (libpq) + libpqxx (no standard find module → locate directly).
find_package(PostgreSQL REQUIRED)
find_path(PQXX_INCLUDE_DIR pqxx/pqxx REQUIRED)
find_library(PQXX_LIBRARY NAMES pqxx REQUIRED)
target_include_directories(engines_cpp PRIVATE ${PQXX_INCLUDE_DIR})
target_link_libraries(engines_cpp PUBLIC ${PQXX_LIBRARY} PostgreSQL::PostgreSQL)

target_link_libraries(engines_cpp PUBLIC
  utilities_cpp
  portfolio_structure_shared
  execution_engine_shared
  position_engine_shared
  hedge_engine_shared
  log_engine_shared
  strategies
  option_strategy_engine_shared
  otrader_proto
  gRPC::grpc++
  protobuf::libprotobuf
)
