# gateway_core: IbGateway + ZMQ schema (for entry_gateway process only)
cmake_minimum_required(VERSION 3.16)
project(gateway_core LANGUAGES CXX)

if(NOT TARGET utilities_cpp)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../utilities ${CMAKE_BINARY_DIR}/utilities)
endif()
if(NOT TARGET otrader_proto)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../proto ${CMAKE_BINARY_DIR}/proto)
endif()

add_library(gateway_core
  engine_gateway_ib.cpp
  zmq_gateway_schema.cpp
)
target_include_directories(gateway_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../utilities
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core
  ${CMAKE_CURRENT_SOURCE_DIR}/../../proto
)
target_link_libraries(gateway_core PUBLIC utilities_cpp otrader_proto)

# TWS (required for gateway)
set(IBJTS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/IBJts")
set(TWS_CLIENT_DIR "${IBJTS_ROOT}/source/cppclient/client")
set(TWS_LIB_DIR "${IBJTS_ROOT}/build-apple/lib")
set(TWS_LIB "${TWS_LIB_DIR}/libtwsapi.dylib")
if(NOT EXISTS "${TWS_CLIENT_DIR}/EClientSocket.h")
  message(FATAL_ERROR "Gateway requires TWS headers at: ${TWS_CLIENT_DIR}")
endif()
if(NOT EXISTS "${TWS_LIB}")
  message(FATAL_ERROR "Gateway requires twsapi at ${TWS_LIB}. Run thirdparty/IBJts build-apple first.")
endif()
add_library(IBJts::twsapi SHARED IMPORTED)
set_target_properties(IBJts::twsapi PROPERTIES
  IMPORTED_LOCATION "${TWS_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${TWS_CLIENT_DIR};${TWS_CLIENT_DIR}/protobufUnix;/opt/homebrew/include"
)
target_link_libraries(gateway_core PRIVATE IBJts::twsapi pthread)
