/**
 * Live 实盘入口。创建 MainEngine + EventEngine，连接 TWS，运行事件循环。
 * 需在运行前设置环境变量（如 DATABASE_URL）：在 repo root 下 source .env 或 export 后执行。
 * 使用方式：先构建 libTwsSocketClient，再 cmake -DBUILD_LIVE=ON && make entry_live
 */

#include "core/engine_log.hpp"
#include "engine_main.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void on_signal(int) { g_running.store(false); }

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    engines::EventEngine event_engine(1);
    engines::MainEngine main_engine(&event_engine);

    main_engine.connect();
    std::cout << "Live engine started. Connect to TWS and run event loop (Ctrl+C to exit).\n"
              << std::flush;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    main_engine.disconnect();
    main_engine.close();
    std::cout << "Live engine stopped.\n" << std::flush;
    return 0;
}
