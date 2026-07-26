# gateway_core: IbGateway + ZMQ schema (used by entry_system --mode=gateway)
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
find_package(Threads REQUIRED)
target_link_libraries(gateway_core PUBLIC utilities_cpp otrader_proto)

# TWS (IBJts). Vendored; the twsapi lib is built under IBJts/${IBJTS_BUILD_DIR} (default
# build-apple). Resolve the lib by name so .dylib/.so both work.
set(IBJTS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/IBJts")
set(TWS_CLIENT_DIR "${IBJTS_ROOT}/source/cppclient/client")
set(IBJTS_BUILD_DIR "build-apple" CACHE STRING "IBJts build subdir")
if(NOT EXISTS "${TWS_CLIENT_DIR}/EClientSocket.h")
  message(FATAL_ERROR "Gateway requires TWS headers at: ${TWS_CLIENT_DIR}")
endif()
find_library(TWS_LIB NAMES twsapi PATHS "${IBJTS_ROOT}/${IBJTS_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
add_library(IBJts::twsapi SHARED IMPORTED)
set_target_properties(IBJts::twsapi PROPERTIES
  IMPORTED_LOCATION "${TWS_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${TWS_CLIENT_DIR};${TWS_CLIENT_DIR}/protobufUnix"
)
target_link_libraries(gateway_core PRIVATE IBJts::twsapi Threads::Threads)
