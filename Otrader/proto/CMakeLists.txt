cmake_minimum_required(VERSION 3.16)

# Protobuf + gRPC C++ code is GENERATED AT BUILD TIME from the .proto sources here.
# Nothing under proto/ except the .proto files is checked in — this keeps the generated
# code ABI-matched to whatever protobuf/gRPC is installed (no version-mismatch surprises
# after a `brew upgrade protobuf grpc`). Requires `protoc` and `grpc_cpp_plugin` on PATH
# (override with -DPROTOC=... / -DGRPC_CPP_PLUGIN=...).

find_program(PROTOC NAMES protoc HINTS /opt/homebrew/bin REQUIRED)
find_program(GRPC_CPP_PLUGIN NAMES grpc_cpp_plugin HINTS /opt/homebrew/opt/grpc/bin /opt/homebrew/bin REQUIRED)

set(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# --- otrader_engine: messages + gRPC service ---
set(ENGINE_PB   "${PROTO_GEN_DIR}/otrader_engine.pb.cc"      "${PROTO_GEN_DIR}/otrader_engine.pb.h")
set(ENGINE_GRPC "${PROTO_GEN_DIR}/otrader_engine.grpc.pb.cc" "${PROTO_GEN_DIR}/otrader_engine.grpc.pb.h")
add_custom_command(
  OUTPUT ${ENGINE_PB} ${ENGINE_GRPC}
  COMMAND ${PROTOC}
    -I "${PROTO_SRC_DIR}"
    --cpp_out "${PROTO_GEN_DIR}"
    --grpc_out "${PROTO_GEN_DIR}"
    "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
    "${PROTO_SRC_DIR}/otrader_engine.proto"
  DEPENDS "${PROTO_SRC_DIR}/otrader_engine.proto"
  COMMENT "protoc: otrader_engine.proto (messages + gRPC)"
  VERBATIM)

# --- zmq_messages: messages only ---
set(ZMQ_PB "${PROTO_GEN_DIR}/zmq_messages.pb.cc" "${PROTO_GEN_DIR}/zmq_messages.pb.h")
add_custom_command(
  OUTPUT ${ZMQ_PB}
  COMMAND ${PROTOC}
    -I "${PROTO_SRC_DIR}"
    --cpp_out "${PROTO_GEN_DIR}"
    "${PROTO_SRC_DIR}/zmq_messages.proto"
  DEPENDS "${PROTO_SRC_DIR}/zmq_messages.proto"
  COMMENT "protoc: zmq_messages.proto (messages)"
  VERBATIM)

# Single library carrying both message code and the gRPC service stubs. Consumers get the
# generated headers via the PUBLIC include dir (the build tree).
add_library(otrader_proto
  "${PROTO_GEN_DIR}/otrader_engine.pb.cc"
  "${PROTO_GEN_DIR}/otrader_engine.grpc.pb.cc"
  "${PROTO_GEN_DIR}/zmq_messages.pb.cc")
target_include_directories(otrader_proto PUBLIC "${PROTO_GEN_DIR}")
target_link_libraries(otrader_proto PUBLIC protobuf::libprotobuf gRPC::grpc++ absl::hash)
