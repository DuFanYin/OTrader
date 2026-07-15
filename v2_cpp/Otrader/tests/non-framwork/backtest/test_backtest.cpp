#include "engine_backtest.hpp"
#include "engine_log.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

// Timestamp to ISO string (align with entry_backtest)
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

    // Explicit parquet path and strategy name
    if (argc < 3) {
        std::cout << "Usage: test_backtest <parquet_path> <strategy_name>\n";
        std::cout << "Example (run from build/tests):\n";
        std::cout << "  ./test_backtest ../../../data/SPXW/SPXW-2025-08/20250804.parquet "
                     "StraddleTestStrategy\n";
        std::cout << "Log: set BACKTEST_LOG=1 to enable strategy/engine log (default off).\n";
        return 1;
    }
    std::string parquet_path = argv[1];
    // Use engine-registered strategy name (strategy_registry REGISTER_STRATEGY)
    std::string strategy_name = argv[2];

    std::cout << std::string(80, '=') << "\n";
    std::cout << "Backtest Engine Single-file Test (C++20)\n";
    std::cout << std::string(80, '=') << "\n";
    std::cout << "Parquet file: " << parquet_path << "\n";
    std::cout << "Strategy: " << strategy_name << "\n";
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
        std::string underlying = "";
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
            {"position_size", 1.0},    {"momentum_threshold", 0.03}, {"profit_target_pct", 0.20},
            {"stop_loss_pct", 0.15},   {"max_holding_minutes", 5.0}, {"iv_change_threshold", 0.005},
            {"cooldown_minutes", 0.0}, {"timer_trigger", 1.0},
        };
        backtest_engine.add_strategy(strategy_name, strategy_setting);

        // Metrics every 10 timesteps
        backtest_engine.register_timestep_callback([&backtest_engine](int timestep, Timestamp) {
            if (timestep % 10 != 0)
                return;
            auto* me = backtest_engine.main_engine();
            if (!me || !me->option_strategy_engine())
                return;
            auto* holding = me->option_strategy_engine()->get_strategy_holding();
            if (!holding)
                return;
            const auto& s = holding->summary;
            std::cout << "  Timestep " << timestep << ": "
                      << "PnL=$" << std::fixed << std::setprecision(2) << s.pnl
                      << ", Delta=" << s.delta << ", Gamma=" << s.gamma << ", Theta=" << s.theta
                      << "\n";
        });

        std::cout << "\n[3] Running backtest (all timesteps)...\n";
        auto t_run_start = std::chrono::steady_clock::now();
        BacktestResult result = backtest_engine.run();
        auto t_run_end = std::chrono::steady_clock::now();
        std::chrono::duration<double> run_sec = t_run_end - t_run_start;
        std::cout << "  Run done. (" << std::fixed << std::setprecision(3) << run_sec.count()
                  << " s)\n";

        std::cout << "\n[4] Backtest Results:\n";
        std::cout << "  Strategy: " << result.strategy_name << "\n";
        std::cout << "  Portfolio: " << result.portfolio_name << "\n";
        std::cout << "  Start:    " << ts_to_iso(result.start_time) << "\n";
        std::cout << "  End:      " << ts_to_iso(result.end_time) << "\n";
        std::cout << "  Timesteps processed: " << result.processed_timesteps << "\n";
        std::cout << "  Frames: " << result.total_frames << ", Rows: " << result.total_rows << "\n";
        std::cout << "  Final P&L: $" << std::fixed << std::setprecision(2) << result.final_pnl
                  << "\n";
        std::cout << "  Max drawdown: $" << std::fixed << std::setprecision(2)
                  << result.max_drawdown << "\n";
        std::cout << "  Total orders: " << result.total_orders << "\n";
        std::cout << "  Max Delta: " << std::setprecision(4) << result.max_delta << "\n";
        std::cout << "  Max Gamma: " << result.max_gamma << "\n";
        std::cout << "  Max Theta: " << result.max_theta << "\n";
        std::cout << "  Load time: " << std::fixed << std::setprecision(3) << load_sec.count()
                  << " s\n";
        std::cout << "  Run time:  " << std::setprecision(3) << run_sec.count() << " s\n";
        if (result.processed_timesteps > 0) {
            double steps_per_sec =
                static_cast<double>(result.processed_timesteps) / run_sec.count();
            std::cout << "  Throughput: " << std::setprecision(1) << steps_per_sec
                      << " timesteps/s\n";
        }
        if (!result.errors.empty()) {
            std::cout << "  Errors:\n";
            for (const auto& e : result.errors)
                std::cout << "    " << e << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << "\nError: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "Test completed\n";
    std::cout << std::string(80, '=') << "\n";
    return 0;
}
