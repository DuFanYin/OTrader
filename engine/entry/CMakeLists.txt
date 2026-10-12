# entry: the executables, and the live one's API: optra_engine.proto and its gRPC service
# (GrpcLiveEngineService), the control plane's way in, outside the cut (core knows no proto).

if(BUILD_BACKTEST)
  add_executable(entry_backtest entry_backtest.cpp)
  target_include_directories(entry_backtest PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/utilities)
  target_link_libraries(entry_backtest PRIVATE core_backtest strategy_factory
    nlohmann_json::nlohmann_json)
endif()

# Live: one process (the market data and the gateway in-process, in MainEngine). BUILD_MARKETDATA
# and BUILD_LIVE_GRPC select the same build, kept apart for compatibility; BUILD_GATEWAY only
# chooses IbGateway (IB) over the base Gateway (no broker): core::live::make_gateway.
if(BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  find_package(absl REQUIRED)
  find_package(gRPC CONFIG REQUIRED)

  # Protobuf + gRPC C++ code is GENERATED AT BUILD TIME from the .proto sources here.
  # Nothing generated is checked in, only the .proto — this keeps the generated
  # code ABI-matched to whatever protobuf/gRPC is installed (no version-mismatch surprises
  # after an apt upgrade of protobuf / gRPC). Requires `protoc` and `grpc_cpp_plugin` on PATH
  # (override with -DPROTOC=... / -DGRPC_CPP_PLUGIN=...).

  find_program(PROTOC NAMES protoc REQUIRED)
  find_program(GRPC_CPP_PLUGIN NAMES grpc_cpp_plugin REQUIRED)

  set(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")

  # --- optra_engine: messages + gRPC service ---
  set(ENGINE_PB   "${PROTO_GEN_DIR}/optra_engine.pb.cc"      "${PROTO_GEN_DIR}/optra_engine.pb.h")
  set(ENGINE_GRPC "${PROTO_GEN_DIR}/optra_engine.grpc.pb.cc" "${PROTO_GEN_DIR}/optra_engine.grpc.pb.h")
  add_custom_command(
    OUTPUT ${ENGINE_PB} ${ENGINE_GRPC}
    COMMAND ${PROTOC}
      -I "${PROTO_SRC_DIR}"
      --cpp_out "${PROTO_GEN_DIR}"
      --grpc_out "${PROTO_GEN_DIR}"
      "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
      "${PROTO_SRC_DIR}/optra_engine.proto"
    DEPENDS "${PROTO_SRC_DIR}/optra_engine.proto"
    COMMENT "protoc: optra_engine.proto (messages + gRPC)"
    VERBATIM)

  # Single library carrying both message code and the gRPC service stubs. Consumers get the
  # generated headers via the PUBLIC include dir (the build tree).
  add_library(optra_proto
    "${PROTO_GEN_DIR}/optra_engine.pb.cc"
    "${PROTO_GEN_DIR}/optra_engine.grpc.pb.cc")
  target_include_directories(optra_proto PUBLIC "${PROTO_GEN_DIR}")
  target_link_libraries(optra_proto PUBLIC protobuf::libprotobuf gRPC::grpc++ absl::hash)

  # The gRPC service over the live engine: the control plane's way in.
  add_library(entry_grpc engine_grpc.cpp)
  target_include_directories(entry_grpc PUBLIC ${CMAKE_SOURCE_DIR})
  target_link_libraries(entry_grpc PUBLIC core_live optra_proto gRPC::grpc++ protobuf::libprotobuf
                        PRIVATE nlohmann_json::nlohmann_json)

  add_executable(entry_live entry_live.cpp)
  target_include_directories(entry_live PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/utilities)
  target_link_libraries(entry_live PRIVATE entry_grpc core_live strategy_factory)

  # The live engine on a simulated market (the public demo): needs the backtest's adapters.
  if(TARGET simulation)
    add_executable(entry_simulate entry_simulate.cpp)
    target_include_directories(entry_simulate PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/utilities)
    target_link_libraries(entry_simulate PRIVATE entry_grpc core_live simulation strategy_factory)
  endif()
endif()

# The executables at the build directory's root, where the scripts, the backend and systemd run them
# (engine/build/entry_backtest, engine/build/entry_live, engine/build/entry_simulate).
foreach(exe entry_backtest entry_live entry_simulate)
  if(TARGET ${exe})
    set_target_properties(${exe} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  endif()
endforeach()
