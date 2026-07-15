/**
 * C++20 equivalent of test_backtest_data.py
 * BacktestEngine-driven; observe backtest data and ATM behavior.
 *
 * Usage: test_backtest_data <parquet_path> [max_timesteps]
 *
 * Without Arrow/Parquet support, load will fail and the test reports "Parquet not available".
 */

#include "engine_backtest.hpp"
#include "engine_log.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

static std::string timestamp_to_string(backtest::Timestamp ts) {
    auto t = std::chrono::system_clock::to_time_t(ts);
    std::ostringstream os;
    os << std::put_time(std::gmtime(&t), "%Y-%m-%d %H:%M:%S");
    return os.str();
}

static void test_load_and_advance_time(
    const std::string& parquet_path, int max_timesteps, const std::string& strategy_name,
    const std::unordered_map<std::string, double>& strategy_setting, bool enable_log) {
    using namespace backtest;

    std::cout << std::string(80, '=') << "\n";
    std::cout << "Backtest Data + ATM Inspection Test (via BacktestEngine)\n";
    std::cout << std::string(80, '=') << "\n";

    BacktestEngine engine;
    int log_level = enable_log ? engines::INFO : engines::DISABLED;
    engine.main_engine()->set_log_level(log_level);

    std::cout << "\n[1] Loading parquet file: " << parquet_path << "\n";
    engine.load_backtest_data(parquet_path);
    BacktestDataEngine* data_engine = engine.data_engine();
    if (!data_engine || !data_engine->has_data()) {
        std::cout << "FAIL Failed to load parquet (or Parquet support not built; use Arrow for "
                     "real data)\n";
        std::cout << "[2] File metadata: (no data)\n";
        std::cout << std::string(80, '=') << "\n";
        return;
    }
    std::cout << "OK Parquet file loaded successfully\n";

    auto meta = data_engine->get_meta();
    std::cout << "\n[2] File metadata:\n";
    std::cout << "  Path: " << meta.path << "\n";
    std::cout << "  Row count: " << meta.row_count << "\n";
    std::cout << "  Time column: " << meta.time_column << "\n";
    std::cout << "  Start time: " << meta.ts_start << "\n";
    std::cout << "  End time: " << meta.ts_end << "\n";

    engine.add_strategy(strategy_name, strategy_setting);

    std::cout << "\n[3] Running backtest and inspecting first " << max_timesteps
              << " timesteps...\n";

    int inspected_timesteps = 0;
    engine.register_timestep_callback([&](int timestep, Timestamp ts) {
        if (timestep > max_timesteps) {
            return;
        }
        ++inspected_timesteps;

        std::cout << "\n  Timestep " << timestep << ": " << timestamp_to_string(ts) << "\n";

        auto* me = engine.main_engine();
        if (!me) {
            return;
        }
        // Portfolio name "backtest"
        auto* p_data = me->get_portfolio("backtest");
        if (!p_data || !p_data->underlying) {
            std::cout << "    (no portfolio/underlying available)\n";
            return;
        }

        double s = p_data->underlying->mid_price;
        std::cout << "    Underlying mid: " << s << "\n";
        std::cout << "    ATM call/put from PortfolioData (S=" << s << "):\n";
        if (s <= 0.0) {
            std::cout << "      (no underlying mid price, cannot select ATM options)\n";
            return;
        }

        // get_chain_by_expiry [0, 3650]
        std::vector<std::string> chain_syms = p_data->get_chain_by_expiry(0, 7);
        int printed_chains = 0;
        for (const auto& chain_sym : chain_syms) {
            if (printed_chains >= 3)
                break;
            const auto* chain = p_data->get_chain(chain_sym);
            if (!chain || chain->atm_index.empty())
                continue;
            auto call_it = chain->calls.find(chain->atm_index);
            auto put_it = chain->puts.find(chain->atm_index);
            if (call_it == chain->calls.end() && put_it == chain->puts.end()) {
                continue;
            }
            std::cout << "      Chain " << chain_sym << " (DTE=" << chain->days_to_expiry
                      << ") ATM strike=" << chain->atm_price << " (index=" << chain->atm_index
                      << ")\n";
            if (call_it != chain->calls.end() && call_it->second) {
                const auto* opt = call_it->second;
                std::cout << "        CALL " << opt->symbol
                          << ": K=" << (opt->strike_price ? *opt->strike_price : 0.0)
                          << " bid=" << opt->bid_price << " ask=" << opt->ask_price
                          << " mid=" << opt->mid_price << " iv=" << opt->mid_iv
                          << " delta=" << opt->delta << " gamma=" << opt->gamma
                          << " theta=" << opt->theta << " vega=" << opt->vega << "\n";
            }
            if (put_it != chain->puts.end() && put_it->second) {
                const auto* opt = put_it->second;
                std::cout << "        PUT  " << opt->symbol
                          << ": K=" << (opt->strike_price ? *opt->strike_price : 0.0)
                          << " bid=" << opt->bid_price << " ask=" << opt->ask_price
                          << " mid=" << opt->mid_price << " iv=" << opt->mid_iv
                          << " delta=" << opt->delta << " gamma=" << opt->gamma
                          << " theta=" << opt->theta << " vega=" << opt->vega << "\n";
            }
            ++printed_chains;
        }
        if (printed_chains == 0) {
            std::cout << "      (no ATM chains available)\n";
        }
    });

    // Run backtest
    BacktestResult result = engine.run();

    std::cout << "\n[4] Summary:\n";
    std::cout << "  Timesteps processed by engine: " << result.processed_timesteps << "\n";
    std::cout << "  Timesteps inspected in callback: " << inspected_timesteps << "\n";
    std::cout << std::string(80, '=') << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2 || (argv[1][0] == '\0')) {
        std::cout << "Usage: test_backtest_data <parquet_path> [max_timesteps]\n";
        std::cout << "  parquet_path   path to parquet file (e.g. "
                     "data/SPXW/SPXW-2025-03/20250303.parquet)\n";
        std::cout << "  max_timesteps  optional, default 5\n";
        std::cout << "Log: set BACKTEST_LOG=1 to enable engine log (default off).\n";
        return 1;
    }
    std::string parquet_path = argv[1];
    int max_timesteps = 5;
    if (argc > 2)
        max_timesteps = std::atoi(argv[2]);
    if (max_timesteps <= 0)
        max_timesteps = 5;

    // Params
    const std::string strategy_name = "StraddleTestStrategy";
    std::unordered_map<std::string, double> strategy_setting = {
        {"position_size", 0.0}, // Minimal position change, drive engine only
    };
    const char* log_env = std::getenv("BACKTEST_LOG");
    bool enable_log = (log_env && (std::string(log_env) == "1" || std::string(log_env) == "true"));

    test_load_and_advance_time(parquet_path, max_timesteps, strategy_name, strategy_setting,
                               enable_log);
    return 0;
}
