/** Live market test: Tradier pull, print 3 chains ATM. */

#include "../../utilities/portfolio.hpp"
#include "engine_main.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

static const int kPullWaitSeconds = 15;
static const int kDrainWaitMs = 2000;

int main() {
    std::cout << "[test_live_market] Starting MainEngine (EventEngine internal)...\n";

    engines::MainEngine main_engine;

    engines::PortfolioStructure* ps = main_engine.portfolio_structure();
    if (!ps) {
        std::cerr << "  FAIL: portfolio_structure is null\n";
        main_engine.close();
        return 1;
    }

    std::vector<std::string> names = ps->get_all_portfolio_names();
    if (names.empty()) {
        std::cout << "  No portfolios (no DB contracts?); skip pull.\n";
        main_engine.close();
        return 0;
    }

    utilities::PortfolioData* port = main_engine.get_portfolio(names[0]);
    if (!port || port->chains.empty()) {
        std::cout << "  Portfolio \"" << names[0] << "\" has no chains; skip pull.\n";
        main_engine.close();
        return 0;
    }

    // Top 3 chains by expiry
    std::vector<std::string> all_chains = port->get_chain_by_expiry(0, 365);
    const size_t n = std::min(size_t(3), all_chains.size());
    if (n == 0) {
        std::cout << "  No chains in DTE [0,365]; skip pull.\n";
        main_engine.close();
        return 0;
    }
    std::vector<std::string> recent_3(all_chains.begin(),
                                      all_chains.begin() + static_cast<ptrdiff_t>(n));

    std::cout << "  Subscribing to " << n << " chain(s):";
    for (const auto& c : recent_3)
        std::cout << " " << c;
    std::cout << "\n";

    main_engine.subscribe_chains("test_live_market", recent_3);
    main_engine.start_market_data_update();

    std::cout << "  Pulling data (wait " << kPullWaitSeconds << "s)...\n";
    std::this_thread::sleep_for(std::chrono::seconds(kPullWaitSeconds));
    main_engine.stop_market_data_update();
    std::this_thread::sleep_for(std::chrono::milliseconds(kDrainWaitMs));

    port = main_engine.get_portfolio(names[0]);
    if (!port) {
        std::cerr << "  Portfolio gone after pull\n";
        main_engine.close();
        return 1;
    }

    std::cout << "\n--- ATM call/put for nearest " << n << " chain(s) ---\n";
    for (const std::string& chain_sym : recent_3) {
        utilities::ChainData* ch = port->get_chain(chain_sym);
        if (!ch) {
            std::cout << "  [" << chain_sym << "] no chain\n";
            continue;
        }
        ch->calculate_atm_price();
        if (ch->atm_index.empty()) {
            std::cout << "  [" << chain_sym << "] no atm_index\n";
            continue;
        }
        auto cit = ch->calls.find(ch->atm_index);
        auto pit = ch->puts.find(ch->atm_index);
        utilities::OptionData* atm_call = (cit != ch->calls.end()) ? cit->second : nullptr;
        utilities::OptionData* atm_put = (pit != ch->puts.end()) ? pit->second : nullptr;
        std::cout << "  " << chain_sym << " (atm_strike=" << ch->atm_price << ")\n";
        if (atm_call) {
            std::cout << "    CALL " << atm_call->symbol << " bid=" << atm_call->bid_price
                      << " ask=" << atm_call->ask_price << " mid=" << atm_call->mid_price
                      << " iv=" << std::fixed << std::setprecision(4) << atm_call->mid_iv
                      << " delta=" << atm_call->delta << " gamma=" << atm_call->gamma
                      << " theta=" << atm_call->theta << " vega=" << atm_call->vega << "\n";
        } else {
            std::cout << "    CALL (none)\n";
        }
        if (atm_put) {
            std::cout << "    PUT  " << atm_put->symbol << " bid=" << atm_put->bid_price
                      << " ask=" << atm_put->ask_price << " mid=" << atm_put->mid_price
                      << " iv=" << std::fixed << std::setprecision(4) << atm_put->mid_iv
                      << " delta=" << atm_put->delta << " gamma=" << atm_put->gamma
                      << " theta=" << atm_put->theta << " vega=" << atm_put->vega << "\n";
        } else {
            std::cout << "    PUT  (none)\n";
        }
    }
    std::cout << "--- done ---\n";

    main_engine.close();
    std::cout << "[test_live_market] Done.\n";
    return 0;
}
