/**
 * Live component load test: Runtime (MainEngine + EventEngine) with ZMQ clients.
 * Validates that all components are created: event/log/db, portfolio_structure,
 * gateway_client, market_data_client, option_strategy_engine, position_engine.
 * CI or local live stack check.
 * With arg "connect" or "1": attempt Gateway connection (requires entry_gateway running).
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
        << "[test_live_components] Runtime (MainEngine + EventEngine, ZMQ Gateway/MarketData)\n"
        << (do_connect ? "  With Gateway connect (entry_gateway must be running)\n"
                       : "  No connect\n");

    engines::MainEngine main_engine;

    if (do_connect) {
        std::cout << "  Connecting to Gateway (ZMQ REP)...\n";
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
    if (!main_engine.gateway_client()) {
        std::cerr << "  FAIL: gateway_client is null\n";
        ok = false;
    }
    if (!main_engine.market_data_client()) {
        std::cerr << "  FAIL: market_data_client is null\n";
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
    std::cout << "  gateway_client OK\n";
    std::cout << "  market_data_client OK\n";
    std::cout << "  option_strategy_engine OK\n";
    std::cout << "  position_engine OK\n";

    main_engine.close();
    std::cout << "[test_live_components] All components loaded and closed successfully.\n";
    return 0;
}
