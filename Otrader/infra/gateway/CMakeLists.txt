# gateway_core: the in-process live gateway. Built in every live configuration; the concrete
# gateway is selected by BUILD_GATEWAY (see live_gateway.hpp):
#   BUILD_GATEWAY=ON  -> IbGateway  (Interactive Brokers TWS; links IBJts, defines OTRADER_WITH_IB)
#   BUILD_GATEWAY=OFF -> NullGateway (logs + rejects orders; zero IBJts dependency)
cmake_minimum_required(VERSION 3.16)
project(gateway_core LANGUAGES CXX)

if(NOT TARGET utilities_cpp)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../utilities ${CMAKE_BINARY_DIR}/utilities)
endif()
if(NOT TARGET otrader_proto)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../proto ${CMAKE_BINARY_DIR}/proto)
endif()

find_package(Threads REQUIRED)

if(BUILD_GATEWAY)
  add_library(gateway_core engine_gateway_ib.cpp)
else()
  add_library(gateway_core engine_gateway_null.cpp)
endif()

target_include_directories(gateway_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../utilities
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core
  ${CMAKE_CURRENT_SOURCE_DIR}/../../proto
)
target_link_libraries(gateway_core PUBLIC utilities_cpp otrader_proto)
target_link_libraries(gateway_core PRIVATE Threads::Threads)

if(BUILD_GATEWAY)
  # OTRADER_WITH_IB is PUBLIC so live_gateway.hpp selects IbGateway everywhere gateway_core is used.
  target_compile_definitions(gateway_core PUBLIC OTRADER_WITH_IB)

  # TWS (IBJts). Vendored; the twsapi lib is built under IBJts/${IBJTS_BUILD_DIR} (default
  # build-apple). Resolve the lib by name so .dylib/.so both work.
  set(IBJTS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/IBJts")
  set(TWS_CLIENT_DIR "${IBJTS_ROOT}/source/cppclient/client")
  set(IBJTS_BUILD_DIR "build-apple" CACHE STRING "IBJts build subdir")
  if(NOT EXISTS "${TWS_CLIENT_DIR}/EClientSocket.h")
    message(FATAL_ERROR "BUILD_GATEWAY=ON requires TWS headers at: ${TWS_CLIENT_DIR}")
  endif()
  find_library(TWS_LIB NAMES twsapi PATHS "${IBJTS_ROOT}/${IBJTS_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
  add_library(IBJts::twsapi SHARED IMPORTED)
  set_target_properties(IBJts::twsapi PROPERTIES
    IMPORTED_LOCATION "${TWS_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${TWS_CLIENT_DIR};${TWS_CLIENT_DIR}/protobufUnix"
  )
  target_link_libraries(gateway_core PRIVATE IBJts::twsapi)
endif()
