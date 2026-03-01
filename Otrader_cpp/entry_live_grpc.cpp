/**
 * entry_live_grpc.cpp
 *
 * gRPC 入口：创建 live EventEngine + MainEngine，并通过 EngineService 暴露给 backend。
 *
 * 注意：
 * - 依赖 gRPC C++ 与由 Otrader/proto/otrader_engine.proto 生成的 otrader_engine.grpc.pb.{h,cc}。
 * - 具体链接 / find_package 逻辑在 CMakeLists.txt 中配置。
 */

#include "engine_event.hpp"
#include "engine_grpc.hpp"
#include "engine_main.hpp"

#include <grpcpp/grpcpp.h>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // 创建 live 事件引擎与主引擎
    engines::EventEngine event_engine(1);
    engines::MainEngine main_engine(&event_engine);

    // 设定 EventEngine 的 main_engine 指针
    event_engine.set_main_engine(&main_engine);

    // 构造 gRPC service，持有 MainEngine*
    engines::GrpcLiveEngineService service(&main_engine);

    // gRPC server 启动
    grpc::ServerBuilder builder;
    // TODO: 端口可以改为配置项；目前默认监听 50051。
    builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::fprintf(stderr, "Failed to start gRPC server on 0.0.0.0:50051\n");
        return 1;
    }

    std::printf("Live gRPC engine listening on 0.0.0.0:50051\n");
    // 阻塞等待，直到进程被外部信号终止
    server->Wait();

    // 结束前确保关闭引擎
    try {
        main_engine.disconnect();
        main_engine.close();
    } catch (...) {
        // 安静失败，避免异常逃出 main
    }

    return 0;
}
