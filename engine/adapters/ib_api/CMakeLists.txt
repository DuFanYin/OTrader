# ib_api (settings, contracts, connection) over the TWS API, which is built here too.
find_package(Threads REQUIRED)

# The TWS API and the Intel decimal library: scripts/fetch.sh ibjts (not in git). Its messages
# come from this build's protoc.
set(THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty")
set(TWS_CLIENT_DIR "${THIRDPARTY_DIR}/IBJts/source/cppclient/client")
set(TWS_PROTO_DIR "${THIRDPARTY_DIR}/IBJts/source/proto")
set(INTEL_BID_LIB "${THIRDPARTY_DIR}/IntelRDFP/libbid.a")
if(NOT EXISTS "${TWS_CLIENT_DIR}/EClientSocket.h" OR NOT EXISTS "${INTEL_BID_LIB}")
  message(FATAL_ERROR "BUILD_GATEWAY=ON needs the TWS API and libbid in ${THIRDPARTY_DIR}; run scripts/fetch.sh ibjts")
endif()

find_program(PROTOC NAMES protoc REQUIRED)
file(GLOB TWS_PROTOS "${TWS_PROTO_DIR}/*.proto")
set(TWS_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/twsapi_proto")
set(TWS_PROTO_OUT)
foreach(proto IN LISTS TWS_PROTOS)
  get_filename_component(name "${proto}" NAME_WE)
  list(APPEND TWS_PROTO_OUT "${TWS_PROTO_GEN_DIR}/${name}.pb.cc" "${TWS_PROTO_GEN_DIR}/${name}.pb.h")
endforeach()
file(MAKE_DIRECTORY "${TWS_PROTO_GEN_DIR}")
add_custom_command(
  OUTPUT ${TWS_PROTO_OUT}
  COMMAND ${PROTOC} -I "${TWS_PROTO_DIR}" --cpp_out "${TWS_PROTO_GEN_DIR}" ${TWS_PROTOS}
  DEPENDS ${TWS_PROTOS}
  COMMENT "protoc: the TWS API's messages"
  VERBATIM)

file(GLOB TWS_SOURCES "${TWS_CLIENT_DIR}/*.cpp")
add_library(twsapi STATIC ${TWS_SOURCES} ${TWS_PROTO_OUT})
set_target_properties(twsapi PROPERTIES CXX_STANDARD 17)
target_include_directories(twsapi SYSTEM PUBLIC "${TWS_CLIENT_DIR}" "${TWS_PROTO_GEN_DIR}")
target_link_libraries(twsapi PUBLIC protobuf::libprotobuf "${INTEL_BID_LIB}" Threads::Threads)

add_library(ib_api ib_api.cpp)
target_link_libraries(ib_api PUBLIC twsapi model)
