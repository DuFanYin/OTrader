/**
 * C++23 equivalent of test_backtest_data.py
 * Driven by the backtest Main; observe backtest data and ATM behavior.
 *
 * Usage: test_backtest_data <parquet_path> [max_timesteps]
 *
 * Without Arrow/Parquet support, load will fail and the test reports "Parquet not available".
 */

#include "tests/lone_backtest.hpp"
#include "utilities/log_level.hpp"
#include <chrono>
#include <cstdlib>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

static std::string timestamp_to_string(model::DateTime ts) {
    auto t = std::chrono::system_clock::to_time_t(ts);
    std::ostringstream os;
    os << std::put_time(std::gmtime(&t), "%Y-%m-%d %H:%M:%S");
    return os.str();
}

static void test_load_and_advance_time(
    const std::string& parquet_path, int max_timesteps, const std::string& strategy_name,
    const std::unordered_map<std::string, double>& strategy_setting, bool enable_log) {
    using namespace backtest;
    using namespace adapters;

    std::cout << std::string(80, '=') << "\n";
    std::cout << "Backtest Data + ATM Inspection Test (via the backtest Main)\n";
    std::cout << std::string(80, '=') << "\n";

    test::LoneBacktest backtest;
    core::Main& engine = backtest.main();
    const utilities::LogLevel log_level = enable_log ? utilities::INFO : utilities::DISABLED;
    engine.log_engine()->set_level(log_level);

    std::cout << "\n[1] Loading parquet file: " << parquet_path << "\n";
    backtest.load_backtest_data(parquet_path);
    HistoricalData* historical = backtest.historical_data();
    if (!historical || !historical->has_data()) {
        std::cout << "FAIL Failed to load parquet (or Parquet support not built; use Arrow for "
                     "real data)\n";
        std::cout << "[2] File metadata: (no data)\n";
        std::cout << std::string(80, '=') << "\n";
        return;
    }
    std::cout << "OK Parquet file loaded successfully\n";
    historical->load_all_chains(); // the whole day inspected, no strategy asking for its chains

    std::cout << "\n[2] File metadata:\n";
    std::cout << "  Path: " << historical->path() << "\n";
    std::cout << "  Row count: " << historical->rows() << "\n";
    std::cout << "  Minutes: " << historical->minutes() << "\n";
    if (const std::size_t n = historical->minutes(); n > 0) {
        using std::chrono::floor, std::chrono::seconds;
        model::PortfolioSnapshot first;
        model::PortfolioSnapshot last;
        historical->fill(0, first);
        historical->fill(n - 1, last);
        std::cout << std::format("  First and last minute: {:%FT%TZ}, {:%FT%TZ}\n",
                                 floor<seconds>(first.datetime), floor<seconds>(last.datetime));
    }

    engine.add_strategy(strategy_name, historical->portfolio_name(), strategy_setting);

    std::cout << "\n[3] Running backtest and inspecting first " << max_timesteps
              << " timesteps...\n";

    int inspected_timesteps = 0;
    const auto inspect = [&](int timestep, model::DateTime ts) {
        if (timestep > max_timesteps) {
            return;
        }
        ++inspected_timesteps;

        std::cout << "\n  Timestep " << timestep << ": " << timestamp_to_string(ts) << "\n";

        // The day's portfolio
        auto* p_data = engine.portfolios()->find_portfolio(historical->portfolio_name());
        if (!p_data || !p_data->underlying()) {
            std::cout << "    (no portfolio/underlying available)\n";
            return;
        }

        double s = p_data->underlying()->mid();
        std::cout << "    Underlying mid: " << s << "\n";
        std::cout << "    ATM call/put from Portfolio (S=" << s << "):\n";
        if (s <= 0.0) {
            std::cout << "      (no underlying mid price, cannot select ATM options)\n";
            return;
        }

        std::vector<std::string> chain_syms = p_data->chains_by_expiry(0, 7);
        int printed_chains = 0;
        for (const auto& chain_sym : chain_syms) {
            if (printed_chains >= 3)
                break;
            const std::optional<model::Chain> chain = p_data->chain(chain_sym);
            if (!chain || chain->atm_strike() <= 0)
                continue;
            const double atm = chain->atm_strike();
            const std::optional<model::Option> call = chain->call(atm);
            const std::optional<model::Option> put = chain->put(atm);
            if (!call && !put) {
                continue;
            }
            std::cout << "      Chain " << chain_sym << " (DTE=" << chain->days_to_expiry()
                      << ") ATM strike=" << atm << "\n";
            for (const auto& [side, opt] : {std::pair{"CALL", call}, std::pair{"PUT ", put}}) {
                if (opt) {
                    std::cout << "        " << side << " " << opt->symbol()
                              << ": K=" << opt->strike() << " bid=" << opt->bid()
                              << " ask=" << opt->ask() << " mid=" << opt->mid()
                              << " iv=" << opt->iv() << " delta=" << opt->delta()
                              << " gamma=" << opt->gamma() << " theta=" << opt->theta()
                              << " vega=" << opt->vega() << "\n";
                }
            }
            ++printed_chains;
        }
        if (printed_chains == 0) {
            std::cout << "      (no ATM chains available)\n";
        }
    };

    if (backtest.begin_day()) {
        for (std::size_t i = 0; i < historical->minutes(); ++i) {
            backtest.run_bar(i);
            inspect(static_cast<int>(i) + 1, backtest.main().find_portfolio(
                                                 historical->portfolio_name())->frame().datetime);
        }
    }
    const DailyResult result = backtest.end_day();

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
    const std::string strategy_name = "RandomBuyStrategy";
    std::unordered_map<std::string, double> strategy_setting = {};
    const char* log_env = std::getenv("BACKTEST_LOG");
    bool enable_log = (log_env && (std::string(log_env) == "1" || std::string(log_env) == "true"));

    test_load_and_advance_time(parquet_path, max_timesteps, strategy_name, strategy_setting,
                               enable_log);
    return 0;
}
