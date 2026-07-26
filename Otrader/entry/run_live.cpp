/**
 * Live mode: gRPC EngineService over the live MainEngine. Talks to the gateway/market
 * processes only via ZMQ clients (GatewayClient/MarketDataClient) — no gateway/IB dependency,
 * so this builds without IBJts.
 */

#include "entry_modes.hpp"

#include "runtime/live/engine_grpc.hpp"
#include "runtime/live/engine_main.hpp"

#include <cstdio>
#include <memory>

#include <grpcpp/grpcpp.h>

namespace entry {

int run_live() {
    engines::MainEngine main_engine;

    // Build gRPC service, holds MainEngine*
    engines::GrpcLiveEngineService service(&main_engine);

    // Start gRPC server
    grpc::ServerBuilder builder;
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

} // namespace entry
