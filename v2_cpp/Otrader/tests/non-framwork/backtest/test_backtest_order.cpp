/**
 * test_backtest_order: print each order for verification.
 * Usage: test_backtest_order <parquet_path> <strategy_name> [n]
 *   strategy_name: from strategy_registry
 *   n: check every n steps (default 1)
 * Example (run from build/tests):
 *   ./test_backtest_order ../../../data/SPXW/SPXW-2025-08/20250804.parquet
 *   StraddleTestStrategy
 */

#include "engine_backtest.hpp"
#include "engine_log.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <set>
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

void print_order(const utilities::OrderData& o) {
    std::cout << "  Order orderid=" << o.orderid << " symbol=" << o.symbol << " direction="
              << (o.direction.has_value() ? utilities::to_string(o.direction.value()) : "?")
              << " type=" << utilities::to_string(o.type) << " volume=" << o.volume
              << " traded=" << o.traded << " status=" << utilities::to_string(o.status)
              << " price=" << o.price << " is_combo=" << (o.is_combo ? "true" : "false");
    if (o.combo_type.has_value()) {
        std::cout << " combo_type=" << utilities::to_string(o.combo_type.value());
    }
    std::cout << "\n";
    if (o.is_combo && o.legs.has_value() && !o.legs->empty()) {
        std::cout << "    Legs: ";
        for (size_t i = 0; i < o.legs->size(); ++i) {
            const auto& leg = (*o.legs)[i];
            if (i != 0U)
                std::cout << " | ";
            std::cout << (leg.symbol.value_or(""));
            if (leg.direction != utilities::Direction::NET)
                std::cout << " " << utilities::to_string(leg.direction);
            std::cout << " ratio=" << leg.ratio;
        }
        std::cout << "\n";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace backtest;

    if (argc < 3) {
        std::cout << "Usage: test_backtest_order <parquet_path> <strategy_name> [n]\n";
        std::cout << "Example (run from build/tests):\n";
        std::cout << "  ./test_backtest_order "
                     "../../../data/SPXW/SPXW-2025-08/20250804.parquet "
                     "StraddleTestStrategy\n";
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
    std::cout << "Backtest Order Trace Test (C++20)\n";
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

    std::set<std::string> printed_orderids;

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

        backtest_engine.register_timestep_callback(
            [&backtest_engine, n, &printed_orderids](int timestep, backtest::Timestamp ts) {
                if (timestep % n != 0)
                    return;
                auto* me = backtest_engine.main_engine();
                if (!me)
                    return;
                auto orders = me->get_all_orders();
                bool any_new = false;
                for (const auto& o : orders) {
                    if (printed_orderids.insert(o.orderid).second) {
                        if (!any_new) {
                            std::cout << "\nTimestep " << timestep << " (" << ts_to_iso(ts)
                                      << ") Orders:\n";
                            any_new = true;
                        }
                        print_order(o);
                    }
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
        std::cout << "  Total orders: " << result.total_orders
                  << " (printed: " << printed_orderids.size() << ")\n";
        std::cout << "  Final PnL: $" << std::fixed << std::setprecision(2) << result.final_pnl
                  << "\n";
    } catch (const std::exception& e) {
        std::cout << "\nError: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "Order test completed\n";
    std::cout << std::string(80, '=') << "\n";
    return 0;
}
