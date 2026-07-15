/**
 * Live strategy-market test: MainEngine, TWS, 7DTE chain, StraddleTestStrategy.
 * Prereqs: DATABASE_URL, TRADIER_TOKEN, TWS running.
 */

#include "../../core/engine_option_strategy.hpp"
#include "../../core/engine_position.hpp"
#include "../../strategy/strategy_registry.hpp"
#include "../../utilities/portfolio.hpp"
#include "engine_main.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;

    std::cout << "[test_live_strategy_straddle] Starting MainEngine (EventEngine internal)...\n";

    engines::MainEngine main_engine;

    // Connect to Gateway process (must run entry_gateway first)
    std::cout << "  Connecting to Gateway (ZMQ)...\n";
    main_engine.connect();
    std::this_thread::sleep_for(3s);
    if (!main_engine.gateway_client() || !main_engine.gateway_client()->is_connected()) {
        std::cerr
            << "  FAIL: Gateway not connected (run entry_gateway first; check GATEWAY_REP_ADDR).\n";
        main_engine.close();
        return 1;
    }

    // 1) Select portfolio
    std::vector<std::string> portfolios = main_engine.get_all_portfolio_names();
    if (portfolios.empty()) {
        std::cout << "  No portfolios loaded (check DATABASE_URL); skip.\n";
        main_engine.close();
        return 0;
    }
    const std::string portfolio_name = portfolios.front();
    utilities::PortfolioData* port = main_engine.get_portfolio(portfolio_name);
    if (!port) {
        std::cerr << "  FAIL: get_portfolio(" << portfolio_name << ") returned null\n";
        main_engine.close();
        return 1;
    }

    // 2) Find DTE=7 chain
    std::vector<std::string> chains7 = port->get_chain_by_expiry(7, 7);
    if (chains7.empty()) {
        std::cout << "  No chains with DTE=7 in portfolio \"" << portfolio_name << "\"; skip.\n";
        main_engine.close();
        return 0;
    }
    const std::string chain_sym = chains7.front();
    std::cout << "  Using portfolio=" << portfolio_name << ", chain_7DTE=" << chain_sym << "\n";

    // 3) Register StraddleTestStrategy
    core::OptionStrategyEngine* se = main_engine.option_strategy_engine();
    if (!se) {
        std::cerr << "  FAIL: option_strategy_engine is null\n";
        main_engine.close();
        return 1;
    }

    const std::string class_name = "StraddleTestStrategy";
    if (!strategy_cpp::StrategyRegistry::has(class_name)) {
        std::cerr << "  FAIL: StrategyRegistry has no " << class_name << "\n";
        main_engine.close();
        return 1;
    }

    std::unordered_map<std::string, double> setting;
    setting["position_size"] = 1.0;
    setting["timer_trigger"] = 1.0; // One on_timer per Timer

    std::cout << "  Adding strategy " << class_name << " on portfolio " << portfolio_name
              << "...\n";
    se->add_strategy(class_name, portfolio_name, setting);
    const std::string strategy_name = class_name + "_" + portfolio_name;

    // 4) Subscribe 7DTE chain
    std::vector<std::string> sub_chains{chain_sym};
    main_engine.subscribe_chains(strategy_name, sub_chains);

    // 5) Init and start strategy
    se->init_strategy(strategy_name);
    se->start_strategy(strategy_name);

    // 6) Start market data, wait for straddle
    constexpr int kWaitSeconds = 5;
    std::cout << "  Starting market data update, wait " << kWaitSeconds
              << "s (+ extra drain) for strategy to place straddle...\n";
    main_engine.start_market_data_update();
    std::this_thread::sleep_for(10s); // Drain order/trade events

    // 7) Trigger PositionEngine metrics update
    if (auto* pos_engine = main_engine.position_engine()) {
        pos_engine->process_timer_event(
            [&main_engine](const std::string& name) -> utilities::PortfolioData* {
                return main_engine.get_portfolio(name);
            });
    }

    // 8) Print holding summary
    utilities::StrategyHolding* holding = main_engine.get_holding(strategy_name);
    if (!holding) {
        std::cout << "  No holding found for strategy " << strategy_name << " (no trades yet?)\n";
    } else {
        std::cout << "  Holding summary: total_cost=" << holding->summary.total_cost
                  << " current_value=" << holding->summary.current_value
                  << " pnl=" << holding->summary.pnl << " delta=" << holding->summary.delta << "\n";
        std::cout << "  Option positions: " << holding->optionPositions.size() << "\n";
        for (const auto& [sym, pos] : holding->optionPositions) {
            std::cout << "    " << sym << " qty=" << pos.quantity << " combo_type=";
            if (pos.combo_type.has_value()) {
                std::cout << utilities::to_string(*pos.combo_type);
            } else {
                std::cout << "single_leg";
            }
            std::cout << "\n";
            // Multi-leg: print each leg
            if (!pos.legs.empty()) {
                for (const auto& leg : pos.legs) {
                    std::cout << "      leg " << leg.symbol << " qty=" << leg.quantity << "\n";
                }
            }
        }
    }

    // Stop market data
    main_engine.stop_market_data_update();

    main_engine.close();
    std::cout << "[test_live_strategy_straddle] Done.\n";
    return 0;
}
