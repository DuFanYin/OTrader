cmake_minimum_required(VERSION 3.16)

# Protobuf + Abseil 由根 CMakeLists 统一 find_package(Homebrew)
add_library(otrader_proto otrader_engine.pb.cc zmq_messages.pb.cc)
target_include_directories(otrader_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(otrader_proto PUBLIC protobuf::libprotobuf absl::hash)
