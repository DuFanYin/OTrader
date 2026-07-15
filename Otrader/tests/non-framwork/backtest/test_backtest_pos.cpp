/**
 * test_backtest_pos: print strategy holding each timestep.
 * Usage: test_backtest_pos <parquet_path> <strategy_name> [n]
 *   strategy_name: from strategy_registry
 *   n: print every n steps (default 1)
 * Example (run from build/tests):
 *   ./test_backtest_pos ../../../data/SPXW/SPXW-2025-08/20250804.parquet
 */

#include "engine_backtest.hpp"
#include "engine_log.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

std::string ts_to_iso(backtest::Timestamp ts) {
    std::time_t t = std::chrono::system_clock::to_time_t(ts);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return os.str();
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace backtest;

    if (argc < 3) {
        std::cout << "Usage: test_backtest_pos <parquet_path> <strategy_name> [n]\n";
        std::cout << "Example (run from build/tests):\n";
        std::cout << "  ./test_backtest_pos "
                     "../../../data/SPXW/SPXW-2025-08/20250804.parquet StraddleTestStrategy\n";
        std::cout << "Log: set BACKTEST_LOG=1 to enable engine log (default off).\n";
        return 1;
    }
    std::string parquet_path = argv[1];
    std::string strategy_name = argv[2];
    int n = 1;
    if (argc > 3) {
        n = std::atoi(argv[3]);
        if (n <= 0)
            n = 1;
    }

    std::cout << std::string(80, '=') << "\n";
    std::cout << "Backtest Position Trace Test (C++20)\n";
    std::cout << std::string(80, '=') << "\n";
    std::cout << "Parquet file: " << parquet_path << "\n";
    std::cout << "Strategy: " << strategy_name << "\n";
    std::cout << "Print interval n: " << n << " timestep(s)\n";
    std::cout << std::string(80, '=') << "\n";

    BacktestEngine backtest_engine;
    const char* log_env = std::getenv("BACKTEST_LOG");
    int log_level = (log_env && (std::string(log_env) == "1" || std::string(log_env) == "true"))
                        ? engines::INFO
                        : engines::DISABLED;
    backtest_engine.main_engine()->set_log_level(log_level);

    try {
        std::cout << "\n[1] Loading data...\n";
        auto t_load_start = std::chrono::steady_clock::now();
        std::string underlying;
        size_t spxw_pos = parquet_path.find("/SPXW/");
        if (spxw_pos != std::string::npos) {
            underlying = "SPXW";
        }
        backtest_engine.load_backtest_data(parquet_path, underlying);
        auto t_load_end = std::chrono::steady_clock::now();
        std::chrono::duration<double> load_sec = t_load_end - t_load_start;
        std::cout << "  Load done. (" << std::fixed << std::setprecision(3) << load_sec.count()
                  << " s)\n";

        std::cout << "\n[2] Adding strategy: " << strategy_name << "...\n";
        std::unordered_map<std::string, double> strategy_setting = {
            {"position_size", 1.0},
            {"timer_trigger", 1.0},
        };
        backtest_engine.add_strategy(strategy_name, strategy_setting);

        // Print full holding every n timesteps
        backtest_engine.register_timestep_callback([&backtest_engine, n](int timestep,
                                                                         Timestamp ts) {
            if (timestep % n != 0)
                return;
            auto* me = backtest_engine.main_engine();
            if (!me || !me->option_strategy_engine())
                return;
            auto* se = me->option_strategy_engine();
            auto* holding = se->get_strategy_holding();
            if (!holding) {
                std::cout << "Timestep " << timestep << " (" << ts_to_iso(ts) << "): no holding\n";
                return;
            }
            const auto& h = *holding;
            const auto& sum = h.summary;

            std::cout << "\nTimestep " << timestep << " (" << ts_to_iso(ts) << ")\n";
            std::cout << "  Summary: PnL=" << sum.pnl << " (unreal=" << sum.unrealized_pnl
                      << ", real=" << sum.realized_pnl << "), "
                      << "Delta=" << sum.delta << ", Gamma=" << sum.gamma << ", Theta=" << sum.theta
                      << ", Vega=" << sum.vega << "\n";

            // Underlying position
            const auto& u = h.underlyingPosition;
            std::cout << "  Underlying: symbol=" << u.symbol << ", qty=" << u.quantity
                      << ", avg_cost=" << u.avg_cost << ", mid=" << u.mid_price << "\n";

            // Option positions (single-leg and multi-leg unified)
            if (!h.optionPositions.empty()) {
                std::cout << "  Option positions:\n";
                for (const auto& [sym, pos] : h.optionPositions) {
                    if (pos.quantity == 0)
                        continue;
                    std::cout << "    " << sym << " : qty=" << pos.quantity
                              << ", avg_cost=" << pos.avg_cost << ", mid=" << pos.mid_price
                              << ", cost=" << pos.cost_value << ", value=" << pos.current_value()
                              << ", realized=" << pos.realized_pnl << "\n";
                    if (!pos.legs.empty()) {
                        std::cout << "      Legs:\n";
                        for (const auto& leg : pos.legs) {
                            if (leg.quantity == 0)
                                continue;
                            std::cout << "        " << leg.symbol << " : qty=" << leg.quantity
                                      << ", avg_cost=" << leg.avg_cost << ", mid=" << leg.mid_price
                                      << ", cost=" << leg.cost_value
                                      << ", value=" << leg.current_value()
                                      << ", realized=" << leg.realized_pnl << "\n";
                        }
                    }
                }
            } else {
                std::cout << "  Option positions: (none)\n";
            }
        });

        std::cout << "\n[3] Running backtest (all timesteps)...\n";
        auto t_run_start = std::chrono::steady_clock::now();
        BacktestResult result = backtest_engine.run();
        auto t_run_end = std::chrono::steady_clock::now();
        std::chrono::duration<double> run_sec = t_run_end - t_run_start;
        std::cout << "  Run done. (" << std::fixed << std::setprecision(3) << run_sec.count()
                  << " s)\n";

        std::cout << "\n[4] Summary:\n";
        std::cout << "  Timesteps processed: " << result.processed_timesteps << "\n";
        std::cout << "  Final PnL: $" << std::fixed << std::setprecision(2) << result.final_pnl
                  << "\n";
        std::cout << "  Total orders: " << result.total_orders << "\n";
    } catch (const std::exception& e) {
        std::cout << "\nError: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "Position test completed\n";
    std::cout << std::string(80, '=') << "\n";
    return 0;
}
