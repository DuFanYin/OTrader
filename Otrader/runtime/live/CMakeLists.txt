cmake_minimum_required(VERSION 3.16)
project(engines_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# gRPC / Protobuf（用于 live gRPC EngineService）
list(APPEND CMAKE_IGNORE_PATH "/opt/anaconda3" "/opt/anaconda3/envs/Dev-env")
list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew")
find_package(gRPC REQUIRED)

# Tradier API: 固定使用 Homebrew（brew install curl nlohmann-json）
set(CURL_HOMEBREW "/opt/homebrew/opt/curl")
add_library(CURL::libcurl SHARED IMPORTED)
set_target_properties(CURL::libcurl PROPERTIES
  IMPORTED_LOCATION "${CURL_HOMEBREW}/lib/libcurl.dylib"
  INTERFACE_INCLUDE_DIRECTORIES "${CURL_HOMEBREW}/include"
)
find_package(nlohmann_json CONFIG REQUIRED)

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

target_link_libraries(engines_cpp PRIVATE pthread CURL::libcurl nlohmann_json::nlohmann_json cppzmq)

# PostgreSQL (libpqxx) for DatabaseEngine
set(PQXX_PREFIX "/opt/homebrew")
set(LIBPQ_PREFIX "/opt/homebrew/opt/libpq")
target_include_directories(engines_cpp PRIVATE ${PQXX_PREFIX}/include)
target_link_directories(engines_cpp PUBLIC ${PQXX_PREFIX}/lib ${LIBPQ_PREFIX}/lib)
target_link_libraries(engines_cpp PUBLIC pqxx pq)

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
