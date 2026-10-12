#include "core/engines/option_strategy.hpp"
#include "../adapters/backtest/engine_data_historical.hpp"
#include "core/engine_backtest.hpp"
#include "../strategy/strategy_registry.hpp"
#include "../utilities/log_level.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

// Largest-Triangle-Three-Buckets downsampling on a single Y series (X = 0..N-1).
// Returns the selected indices (0-based). threshold includes first/last; if N <= threshold,
// returns [0..N-1].
std::vector<size_t> lttb_downsample_indices(const std::vector<double>& y, size_t threshold) {
    const size_t N = y.size();
    if (threshold >= N || threshold < 3 || N == 0) {
        std::vector<size_t> idx(N);
        for (size_t i = 0; i < N; ++i) {
            idx[i] = i;
        }
        return idx;
    }

    std::vector<size_t> out;
    out.reserve(threshold);
    // Always include first point
    out.push_back(0);

    const double bucket_size = static_cast<double>(N - 2) / static_cast<double>(threshold - 2);
    size_t a = 0;

    for (size_t i = 0; i < threshold - 2; ++i) {
        const double bucket_start = 1.0 + static_cast<double>(i) * bucket_size;
        const double bucket_end = bucket_start + bucket_size;

        const size_t start = static_cast<size_t>(std::floor(bucket_start));
        const size_t end = std::min(static_cast<size_t>(std::floor(bucket_end)), N - 1);

        // Calculate average for next bucket (used as point C)
        const double next_bucket_start = bucket_end;
        const double next_bucket_end = next_bucket_start + bucket_size;
        const size_t next_start = static_cast<size_t>(std::floor(next_bucket_start));
        const size_t next_end = std::min(static_cast<size_t>(std::floor(next_bucket_end)), N - 1);

        double avg_x = 0.0;
        double avg_y = 0.0;
        size_t avg_count = 0;
        for (size_t j = next_start; j < next_end; ++j) {
            avg_x += static_cast<double>(j);
            avg_y += y[j];
            ++avg_count;
        }
        if (avg_count == 0) {
            avg_x = static_cast<double>(a);
            avg_y = y[a];
        } else {
            avg_x /= static_cast<double>(avg_count);
            avg_y /= static_cast<double>(avg_count);
        }

        // Point A
        const double ax = static_cast<double>(a);
        const double ay = y[a];

        // Find the point in this bucket that forms the largest triangle area with A and avg point C
        double max_area = -1.0;
        size_t selected = start;
        for (size_t j = start; j < end; ++j) {
            const double bx = static_cast<double>(j);
            const double by = y[j];
            const double area = std::abs((ax - avg_x) * (by - ay) - (ax - bx) * (avg_y - ay));
            if (area > max_area) {
                max_area = area;
                selected = j;
            }
        }

        out.push_back(selected);
        a = selected;
    }

    // Always include last point
    out.push_back(N - 1);
    return out;
}

std::string ts_to_iso(adapters::Timestamp ts) {
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

// The result goes to stdout and nothing else does: main moves stdout to a descriptor of its own
// (result_fd) and points fd 1 at stderr, so the logs, and anything else printed, go there.
int result_fd = STDOUT_FILENO;

void print_json(const nlohmann::json& out) {
    const std::string text = out.dump();
    std::cout << std::flush;
    for (std::size_t done = 0; done < text.size();) {
        const ssize_t n = ::write(result_fd, text.data() + done, text.size() - done);
        if (n <= 0) {
            return;
        }
        done += static_cast<std::size_t>(n);
    }
}

void print_error_json(const std::string& msg) {
    print_json({{"status", "error"}, {"error", msg}});
}

// Every registered strategy with its parameters: the default a run uses and the range a strategy
// search may vary it in.
void print_strategies_json() {
    nlohmann::json classes = nlohmann::json::array();
    for (const std::string& name : strategy::StrategyRegistry::get_all_strategy_class_names()) {
        nlohmann::json parameters = nlohmann::json::array();
        for (const strategy::StrategyParam& p : strategy::StrategyRegistry::parameters(name)) {
            parameters.push_back({{"name", std::string(p.name)},
                                  {"default", p.default_value},
                                  {"min", p.min},
                                  {"max", p.max},
                                  {"integer", p.integer}});
        }
        classes.push_back({{"name", name}, {"parameters", std::move(parameters)}});
    }
    print_json({{"status", "ok"}, {"strategies", std::move(classes)}});
}

/** The P&L path across days: each day's curve from its own start, shifted by the days before's
 * closes; and where each day after the first starts. */
struct StitchedPnl {
    std::vector<double> pnl;
    std::vector<int> day_starts;
};

auto stitch_days(const std::vector<core::backtest::Metric>& metrics) -> StitchedPnl {
    StitchedPnl out;
    out.pnl.reserve(metrics.size());
    std::string prev_date;
    double offset = 0.0; // the days before's net P&L
    double day_start = 0.0;
    for (size_t i = 0; i < metrics.size(); ++i) {
        const std::string date = metrics[i].timestamp.substr(0, 10);
        if (date != prev_date) {
            if (!prev_date.empty()) {
                offset += metrics[i - 1].pnl - day_start;
                out.day_starts.push_back(static_cast<int>(i));
            }
            prev_date = date;
            day_start = metrics[i].pnl;
        }
        out.pnl.push_back(metrics[i].pnl - day_start + offset);
    }
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    if (const int fd = ::dup(STDOUT_FILENO); fd >= 0 && ::dup2(STDERR_FILENO, STDOUT_FILENO) >= 0) {
        result_fd = fd;
    }
    if (argc == 2 && std::string(argv[1]) == "--list-strategies") {
        try {
            print_strategies_json();
            return 0;
        } catch (const std::exception& e) {
            print_error_json(e.what());
            return 1;
        }
    }
    if (argc < 3) {
        print_error_json(
            "Usage: backtest_entry <parquet_path>|<--files file1 file2 ...> <strategy_name> "
            "[--fee-rate number] [--slippage-bps number] "
            "[--risk-free-rate number] [--iv-price-mode mid|bid|ask] [--log] [key=value ...] "
            "[risk.limit=value ...] | "
            "backtest_entry --list-strategies");
        return 1;
    }

    std::vector<std::string> parquet_files;
    std::string strategy_name;
    double fee_rate = 0.35;
    double slippage_bps = 5.0;
    double risk_free_rate = 0.05;
    std::string iv_price_mode = "mid";
    utilities::LogLevel log_level = utilities::DISABLED;
    {
        const char* log_env = std::getenv("BACKTEST_LOG");
        if (log_env && (std::string(log_env) == "1" || std::string(log_env) == "true"))
            log_level = utilities::INFO;
    }
    std::unordered_map<std::string, double> strategy_setting;
    std::unordered_map<std::string, double> risk_setting; // risk.<limit>=<value>

    // Parse arguments: check if first arg is --files
    int arg_idx = 1;
    if (std::string(argv[1]) == "--files") {
        // Multi-file mode: collect file paths until we hit strategy_name
        arg_idx = 2;
        while (arg_idx < argc) {
            const std::string arg = argv[arg_idx];
            // Stop if we hit a flag or key=value (strategy_name should come before these)
            if (arg == "--fee-rate" || arg == "--slippage-bps" || arg == "--risk-free-rate" ||
                arg == "--iv-price-mode" || arg == "--log" || arg.find('=') != std::string::npos) {
                break;
            }
            // Check if it looks like a strategy name (no slash, no dot)
            if (arg.find('/') == std::string::npos && arg.find('.') == std::string::npos &&
                arg.find('\\') == std::string::npos) {
                // Likely strategy name
                strategy_name = arg;
                arg_idx++;
                break;
            }
            parquet_files.push_back(arg);
            arg_idx++;
        }
    } else {
        // Single file mode
        parquet_files.push_back(argv[1]);
        strategy_name = argv[2];
        arg_idx = 3;
    }

    if (parquet_files.empty()) {
        print_error_json("No parquet files specified");
        return 1;
    }
    if (strategy_name.empty()) {
        print_error_json("Strategy name not specified");
        return 1;
    }

    // Parse remaining arguments
    for (int i = arg_idx; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--fee-rate" && i + 1 < argc) {
            try {
                fee_rate = std::stod(argv[++i]);
            } catch (...) {
                // Keep default if invalid fee-rate.
            }
            continue;
        }
        if (arg == "--slippage-bps" && i + 1 < argc) {
            try {
                slippage_bps = std::stod(argv[++i]);
                if (slippage_bps < 0.0) {
                    slippage_bps = 0.0;
                }
            } catch (...) {
                // Keep default if invalid.
            }
            continue;
        }
        if (arg == "--risk-free-rate" && i + 1 < argc) {
            try {
                risk_free_rate = std::stod(argv[++i]);
            } catch (...) {
                // Keep default if invalid value.
            }
            continue;
        }
        if (arg == "--iv-price-mode" && i + 1 < argc) {
            iv_price_mode = argv[++i];
            continue;
        }
        if (arg == "--log") {
            log_level = utilities::INFO;
            continue;
        }

        // Strategy setting key=value; anything else is a mistake, not something to skip.
        std::string kv = argv[i];
        auto pos = kv.find('=');
        if (pos == std::string::npos || pos == 0 || pos == kv.size() - 1) {
            print_error_json("Unknown argument: " + kv);
            return 1;
        }
        std::string key = kv.substr(0, pos);
        std::string val = kv.substr(pos + 1);
        try {
            size_t used = 0;
            const double value = std::stod(val, &used);
            if (key.starts_with("risk.")) {
                risk_setting[key.substr(5)] = value;
            } else {
                strategy_setting[key] = value;
            }
            if (used != val.size()) {
                throw std::invalid_argument(val);
            }
        } catch (...) {
            print_error_json("Setting " + kv + " is not a number");
            return 1;
        }
    }

    try {
        core::backtest::BacktestRunSummary summary = core::backtest::run_backtest_multi(
            parquet_files, strategy_name, fee_rate, slippage_bps, risk_free_rate, iv_price_mode,
            log_level, strategy_setting, risk_setting);

        auto& metrics = summary.metrics;
        auto& daily_results = summary.daily_results;
        auto& daily_returns = summary.daily_returns;
        core::backtest::BacktestResult aggregated_result = summary.aggregated_result;
        double duration_seconds = summary.duration_seconds;
        auto duration_ms = summary.duration_ms;

        // Filter out empty daily_results (no file_path) and days whose file failed to load
        // (their reason is already in result.errors).
        std::vector<core::backtest::DailyResult> sorted_daily_results;
        sorted_daily_results.reserve(daily_results.size());
        for (const auto& daily : daily_results) {
            if (!daily.file_path.empty() && !daily.load_failed) {
                sorted_daily_results.push_back(daily);
            }
        }

        double total_fees = 0.0;
        for (const auto& daily : sorted_daily_results) {
            total_fees += daily.daily_fees;
        }

        const StitchedPnl path = stitch_days(metrics);
        double max_drawdown = 0.0;
        double peak = path.pnl.empty() ? 0.0 : path.pnl.front();
        for (const double value : path.pnl) {
            peak = std::max(peak, value);
            max_drawdown = std::max(max_drawdown, peak - value);
        }
        aggregated_result.max_drawdown = max_drawdown;
        const core::backtest::BacktestResult& result = aggregated_result;

        // Daily Sharpe from daily_returns (net PnL per file, already in file_index order)
        double daily_sharpe = 0.0;
        std::erase_if(daily_returns, [](double r) { return !std::isfinite(r); }); // failed loads
        if (daily_returns.size() > 1) {
            double mean = 0.0;
            for (double ret : daily_returns)
                mean += ret;
            mean /= static_cast<double>(daily_returns.size());
            double var = 0.0;
            for (double ret : daily_returns) {
                const double d = ret - mean;
                var += d * d;
            }
            var /= static_cast<double>(daily_returns.size() - 1);
            const double stdv = std::sqrt(var);
            if (stdv > 1e-12)
                daily_sharpe = mean / stdv * std::sqrt(252.0);
        }

        nlohmann::json days = nlohmann::json::array();
        for (const auto& daily : sorted_daily_results) {
            // The day's fills: what a per-trade report (POST /api/backtest/report) reads.
            nlohmann::json trades = nlohmann::json::array();
            for (const model::TradeData& trade : daily.result.trades) {
                trades.push_back(
                    {{"symbol", trade.symbol},
                     {"orderid", trade.orderid},
                     {"direction", model::to_string(trade.direction)},
                     {"price", trade.price},
                     {"volume", trade.volume},
                     {"time", trade.datetime != model::DateTime{}
                                  ? std::format("{:%Y-%m-%d %H:%M:%S}",
                                                std::chrono::floor<std::chrono::seconds>(
                                                    trade.datetime))
                                  : std::string{}}});
            }
            days.push_back({{"file", daily.file_path},
                            {"pnl", daily.daily_pnl},
                            {"net_pnl", daily.daily_pnl - daily.daily_fees},
                            {"fees", daily.daily_fees},
                            {"orders", daily.result.total_orders},
                            {"timesteps", daily.result.processed_timesteps},
                            {"rows", daily.result.total_rows},
                            {"attribution",
                             {{"delta", daily.summary.pnl_delta},
                              {"gamma", daily.summary.pnl_gamma},
                              {"theta", daily.summary.pnl_theta},
                              {"vega", daily.summary.pnl_vega},
                              {"other", daily.summary.pnl_other},
                              {"fills", daily.summary.pnl_fills}}},
                            {"trades", std::move(trades)}});
        }

        // Chart-ready: the stitched P&L and the Greeks at the same points, downsampled (LTTB on
        // the P&L) to at most 1000 so the page only draws.
        constexpr size_t kMaxChartPoints = 1000;
        nlohmann::json chart = {{"pnl", nlohmann::json::array()},
                                {"x_greek", nlohmann::json::array()},
                                {"delta", nlohmann::json::array()},
                                {"theta", nlohmann::json::array()},
                                {"gamma", nlohmann::json::array()},
                                {"day_boundaries", path.day_starts}};
        for (const size_t idx : lttb_downsample_indices(path.pnl, kMaxChartPoints)) {
            chart["pnl"].push_back(path.pnl[idx]);
            chart["x_greek"].push_back(static_cast<int>(idx));
            chart["delta"].push_back(metrics[idx].delta);
            chart["theta"].push_back(metrics[idx].theta);
            chart["gamma"].push_back(metrics[idx].gamma);
        }

        const nlohmann::json out = {
            {"status", "ok"},
            {"result",
             {{"strategy_name", result.strategy_name},
              {"portfolio_name", result.portfolio_name},
              {"start_time", ts_to_iso(result.start_time)},
              {"end_time", ts_to_iso(result.end_time)},
              {"total_timesteps", result.total_timesteps},
              {"processed_timesteps", result.processed_timesteps},
              {"total_frames", result.total_frames},
              {"total_rows", result.total_rows},
              {"total_orders", result.total_orders},
              {"max_delta", result.max_delta},
              {"max_gamma", result.max_gamma},
              {"max_theta", result.max_theta},
              {"max_drawdown", result.max_drawdown},
              {"daily_sharpe", daily_sharpe},
              {"total_fees", total_fees},
              {"fill_mode", "buy=ask,sell=bid"},
              {"fee_rate", fee_rate},
              {"risk_free_rate", risk_free_rate},
              {"iv_price_mode", iv_price_mode},
              {"final_pnl", result.final_pnl},
              {"net_pnl", result.final_pnl - total_fees},
              {"num_days", sorted_daily_results.size()},
              {"duration_seconds", duration_seconds},
              {"duration_ms", duration_ms}}},
            {"daily_results", std::move(days)},
            {"chart_data", std::move(chart)},
            {"errors", result.errors}};
        print_json(out);
        return 0;
    } catch (const std::exception& e) {
        print_error_json(e.what());
        return 1;
    } catch (...) {
        print_error_json("Unknown C++ backtest error");
        return 1;
    }
}
