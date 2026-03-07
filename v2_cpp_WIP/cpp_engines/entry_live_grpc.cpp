/**
 * entry_live_grpc.cpp
 *
 * gRPC entry: create live MainEngine (holds EventEngine), expose via EngineService.
 * Depends on gRPC C++ and otrader_engine.grpc.pb from proto; CMakeLists.txt for linking.
 */

#include "engine_grpc.hpp"
#include "engine_main.hpp"

#include <grpcpp/grpcpp.h>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    engines::MainEngine main_engine;

    // Build gRPC service, holds MainEngine*
    engines::GrpcLiveEngineService service(&main_engine);

    // Start gRPC server
    grpc::ServerBuilder builder;
    // Port 50051
    builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::fprintf(stderr, "Failed to start gRPC server on 0.0.0.0:50051\n");
        return 1;
    }

    std::printf("Live gRPC engine listening on 0.0.0.0:50051\n");
    // Block until external signal
    server->Wait();

    // Close engine before exit
    try {
        main_engine.disconnect();
        main_engine.close();
    } catch (...) {
        // Fail quietly, avoid exception from main
    }

    return 0;
}
