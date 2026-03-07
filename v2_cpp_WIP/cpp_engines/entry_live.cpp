/** Live entry: MainEngine, TWS, event loop. */

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

    engines::MainEngine main_engine;

    // No auto-connect; external controls connect/disconnect
    std::cout << "Live engine started (idle). Use explicit connect/disconnect controls.\n"
              << std::flush;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    main_engine.close();
    std::cout << "Live engine stopped.\n" << std::flush;
    return 0;
}
