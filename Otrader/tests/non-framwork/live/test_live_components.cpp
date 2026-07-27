/**
 * Live component load test: single-process Runtime (MainEngine + EventEngine, in-process
 * market-data + gateway). Validates that all components are created: event/log/db,
 * portfolio_structure, gateway, option_strategy_engine, position_engine. CI or local check.
 * With arg "connect" or "1": attempt gateway connection (NullGateway is a no-op without IB).
 */

#include "engine_main.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    bool do_connect = false;
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "1" || arg == "true" || arg == "connect") {
            do_connect = true;
        }
    }

    std::cout
        << "[test_live_components] Runtime (MainEngine + EventEngine, in-process gateway/market)\n"
        << (do_connect ? "  With gateway connect\n" : "  No connect\n");

    engines::MainEngine main_engine;

    if (do_connect) {
        std::cout << "  Connecting gateway...\n";
        main_engine.connect();
    }

    bool ok = true;
    if (!main_engine.event_engine()) {
        std::cerr << "  FAIL: event_engine is null\n";
        ok = false;
    }
    if (!main_engine.log_engine()) {
        std::cerr << "  FAIL: log_engine is null\n";
        ok = false;
    }
    if (!main_engine.db_engine()) {
        std::cerr << "  FAIL: db_engine is null\n";
        ok = false;
    }
    if (!main_engine.portfolio_structure()) {
        std::cerr << "  FAIL: portfolio_structure is null\n";
        ok = false;
    }
    if (!main_engine.gateway()) {
        std::cerr << "  FAIL: gateway is null\n";
        ok = false;
    }
    if (!main_engine.option_strategy_engine()) {
        std::cerr << "  FAIL: option_strategy_engine is null\n";
        ok = false;
    }
    if (!main_engine.position_engine()) {
        std::cerr << "  FAIL: position_engine is null\n";
        ok = false;
    }

    if (!ok) {
        main_engine.close();
        return 1;
    }

    std::cout << "  event_engine OK\n";
    std::cout << "  log_engine OK\n";
    std::cout << "  db_engine OK\n";
    std::cout << "  portfolio_structure OK\n";
    std::cout << "  gateway OK\n";
    std::cout << "  option_strategy_engine OK\n";
    std::cout << "  position_engine OK\n";

    main_engine.close();
    std::cout << "[test_live_components] All components loaded and closed successfully.\n";
    return 0;
}
