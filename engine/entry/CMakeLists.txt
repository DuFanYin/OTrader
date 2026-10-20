# The programs, and the live engine's gRPC service (core knows no proto).

if(BUILD_BACKTEST)
  add_executable(entry_backtest entry_backtest.cpp)
  target_include_directories(entry_backtest PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(entry_backtest PRIVATE backtest nlohmann_json::nlohmann_json)
  optra_strategy_host(entry_backtest)
  optra_code_identity(entry_backtest ENGINE core CORE entry_backtest)

  add_executable(entry_research entry_research.cpp)
  target_link_libraries(entry_research PRIVATE research nlohmann_json::nlohmann_json)
endif()

# Live: one process. BUILD_GATEWAY chooses IbGateway over no broker.
if(BUILD_GATEWAY OR BUILD_MARKETDATA OR BUILD_LIVE_GRPC)
  find_package(absl REQUIRED)
  find_package(gRPC CONFIG REQUIRED)

  # The protobuf and gRPC code is generated at build time, matching what is installed.
  find_program(PROTOC NAMES protoc REQUIRED)
  find_program(GRPC_CPP_PLUGIN NAMES grpc_cpp_plugin REQUIRED)

  set(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")

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

  add_library(optra_proto
    "${PROTO_GEN_DIR}/optra_engine.pb.cc"
    "${PROTO_GEN_DIR}/optra_engine.grpc.pb.cc")
  target_include_directories(optra_proto PUBLIC "${PROTO_GEN_DIR}")
  target_link_libraries(optra_proto PUBLIC protobuf::libprotobuf gRPC::grpc++ absl::hash)

  add_library(entry_grpc engine_grpc.cpp)
  target_include_directories(entry_grpc PUBLIC ${CMAKE_SOURCE_DIR})
  target_link_libraries(entry_grpc PUBLIC live optra_proto gRPC::grpc++ protobuf::libprotobuf
                        PRIVATE nlohmann_json::nlohmann_json)

  add_executable(entry_live entry_live.cpp)
  target_include_directories(entry_live PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(entry_live PRIVATE entry_grpc live)
  optra_strategy_host(entry_live)
  optra_code_identity(entry_live ENGINE core)

  # The live engine on a simulated market (the public demo).
  if(BUILD_BACKTEST)
    add_executable(entry_simulate entry_simulate.cpp)
    target_include_directories(entry_simulate PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(entry_simulate PRIVATE entry_grpc live simulated_gateway
                          simulated_market_data data_loader)
    optra_strategy_host(entry_simulate)
    optra_code_identity(entry_simulate ENGINE core)
  endif()
endif()

# The programs at the build directory's root, where everything runs them.
foreach(exe entry_backtest entry_live entry_simulate entry_research)
  if(TARGET ${exe})
    set_target_properties(${exe} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  endif()
endforeach()
