#include "core/engine_log.hpp"
#include "core/engine_option_strategy.hpp"
#include "engine_backtest.hpp"
#include "engine_data_historical.hpp"
#include "engine_main.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

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

void print_error_json(const std::string& msg) {
    std::cout << "{\"status\":\"error\",\"error\":\"" << json_escape(msg) << "\"}" << std::flush;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_error_json(
            "Usage: backtest_entry <parquet_path>|<--files file1 file2 ...> <strategy_name> "
            "[--fee-rate number] [--slippage-bps number] "
            "[--risk-free-rate number] [--iv-price-mode mid|bid|ask] [--log] [key=value ...]");
        return 1;
    }

    std::vector<std::string> parquet_files;
    std::string strategy_name;
    double fee_rate = 0.35;
    double slippage_bps = 5.0;
    double risk_free_rate = 0.05;
    std::string iv_price_mode = "mid";
    int log_level = engines::DISABLED;
    {
        const char* log_env = std::getenv("BACKTEST_LOG");
        if (log_env && (std::string(log_env) == "1" || std::string(log_env) == "true"))
            log_level = engines::INFO;
    }
    std::unordered_map<std::string, double> strategy_setting;

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
            log_level = engines::INFO;
            continue;
        }

        std::string kv = argv[i];
        auto pos = kv.find('=');
        if (pos == std::string::npos || pos == 0 || pos == kv.size() - 1)
            continue;
        std::string key = kv.substr(0, pos);
        std::string val = kv.substr(pos + 1);
        try {
            strategy_setting[key] = std::stod(val);
        } catch (...) {
            // Ignore non-numeric strategy settings in C++ runner.
        }
    }

    try {
        backtest::BacktestRunSummary summary = backtest::run_backtest_multi(
            parquet_files, strategy_name, fee_rate, slippage_bps, risk_free_rate, iv_price_mode,
            log_level, strategy_setting);

        auto& metrics = summary.metrics;
        auto& daily_results = summary.daily_results;
        auto& daily_returns = summary.daily_returns;
        backtest::BacktestResult aggregated_result = summary.aggregated_result;
        double duration_seconds = summary.duration_seconds;
        auto duration_ms = summary.duration_ms;

        // Filter out empty daily_results (no file_path)
        std::vector<backtest::DailyResult> sorted_daily_results;
        sorted_daily_results.reserve(daily_results.size());
        for (const auto& daily : daily_results) {
            if (!daily.file_path.empty()) {
                sorted_daily_results.push_back(daily);
            }
        }

        double total_fees = 0.0;
        for (const auto& daily : sorted_daily_results) {
            total_fees += daily.daily_fees;
        }

        // Compute max drawdown over the same cross-day PnL path that we use for chart_data.
        // Backtest engine is responsible for the *intra-day* PnL path; here we only stitch
        // days together by shifting each day's curve by the previous day's close.
        double max_drawdown = 0.0;
        if (!metrics.empty()) {
            std::string prev_date;
            double cumulative_offset = 0.0; // accumulated previous days' net PnL
            double day_start_pnl = 0.0;
            bool have_value = false;
            double peak = 0.0;

            for (size_t i = 0; i < metrics.size(); ++i) {
                std::string date_str;
                if (metrics[i].timestamp.size() >= 10) {
                    date_str = metrics[i].timestamp.substr(0, 10);
                }
                if (date_str != prev_date) {
                    if (!prev_date.empty() && i > 0) {
                        // Shift next day's curve by previous day's close (relative to its own
                        // start).
                        cumulative_offset += metrics[i - 1].pnl - day_start_pnl;
                    }
                    prev_date = date_str;
                    day_start_pnl = metrics[i].pnl;
                }
                // Intra-day PnL relative to this day's start, then shifted by prior days' closes.
                double value = (metrics[i].pnl - day_start_pnl) + cumulative_offset;
                if (!have_value) {
                    peak = value;
                    have_value = true;
                }
                peak = std::max(peak, value);
                max_drawdown = std::max(max_drawdown, peak - value);
            }
        }
        aggregated_result.max_drawdown = max_drawdown;

        backtest::BacktestResult result = aggregated_result;

        // Daily Sharpe from daily_returns (net PnL per file, already in file_index order)
        double daily_sharpe = 0.0;
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

        std::ostringstream out;
        out << "{\"status\":\"ok\",";
        out << "\"result\":{";
        out << "\"strategy_name\":\"" << json_escape(result.strategy_name) << "\",";
        out << "\"portfolio_name\":\"" << json_escape(result.portfolio_name) << "\",";
        out << "\"start_time\":\"" << ts_to_iso(result.start_time) << "\",";
        out << "\"end_time\":\"" << ts_to_iso(result.end_time) << "\",";
        out << "\"total_timesteps\":" << result.total_timesteps << ",";
        out << "\"processed_timesteps\":" << result.processed_timesteps << ",";
        out << "\"total_frames\":" << result.total_frames << ",";
        out << "\"total_rows\":" << result.total_rows << ",";
        out << "\"total_orders\":" << result.total_orders << ",";
        out << "\"max_delta\":" << result.max_delta << ",";
        out << "\"max_gamma\":" << result.max_gamma << ",";
        out << "\"max_theta\":" << result.max_theta << ",";
        out << "\"max_drawdown\":" << result.max_drawdown << ",";
        out << "\"daily_sharpe\":" << daily_sharpe << ",";
        out << "\"total_fees\":" << total_fees << ",";
        out << "\"fill_mode\":\"buy=ask,sell=bid\",";
        out << "\"fee_rate\":" << fee_rate << ",";
        out << "\"risk_free_rate\":" << risk_free_rate << ",";
        out << "\"iv_price_mode\":\"" << json_escape(iv_price_mode) << "\",";
        out << "\"final_pnl\":" << result.final_pnl << ",";
        double net_pnl = result.final_pnl - total_fees;
        out << "\"net_pnl\":" << net_pnl << ",";
        out << "\"num_days\":" << sorted_daily_results.size() << ",";
        out << "\"duration_seconds\":" << std::fixed << std::setprecision(3) << duration_seconds
            << ",";
        out << "\"duration_ms\":" << duration_ms;
        out << "},";

        // Add daily results array (already sorted by file_index)
        out << "\"daily_results\":[";
        for (size_t i = 0; i < sorted_daily_results.size(); ++i) {
            const auto& daily = sorted_daily_results[i];
            if (i > 0)
                out << ",";
            double daily_net_pnl = daily.daily_pnl - daily.daily_fees;
            out << "{";
            out << "\"file\":\"" << json_escape(daily.file_path) << "\",";
            out << "\"pnl\":" << daily.daily_pnl << ",";
            out << "\"net_pnl\":" << daily_net_pnl << ",";
            out << "\"fees\":" << daily.daily_fees << ",";
            out << "\"orders\":" << daily.result.total_orders << ",";
            out << "\"timesteps\":" << daily.result.processed_timesteps << ",";
            out << "\"rows\":" << daily.result.total_rows;
            out << "}";
        }
        out << "],";

        // Chart-ready data: cumulative PnL (per-day reset) + Greeks so Python only draws.
        // All four series (PnL / Delta / Theta / Gamma) share the same LTTB downsample indices.
        std::vector<double> chart_pnl;
        std::vector<int> chart_x_greek;
        std::vector<double> chart_delta, chart_theta, chart_gamma;
        std::vector<int> day_boundaries;
        const size_t n_metrics = metrics.size();
        if (n_metrics > 0) {
            // Build full PnL path with per-day reset + cross-day stacking (same as before)
            std::vector<double> full_pnl;
            full_pnl.reserve(n_metrics);
            std::string prev_date;
            double cumulative_offset = 0.0;
            double day_start_pnl = 0.0;
            for (size_t i = 0; i < n_metrics; ++i) {
                std::string date_str;
                if (metrics[i].timestamp.size() >= 10) {
                    date_str = metrics[i].timestamp.substr(0, 10);
                }
                if (date_str != prev_date) {
                    if (!prev_date.empty() && i > 0) {
                        // metrics[i-1].pnl is cumulative from start; day_start_pnl was start of
                        // previous day So previous day's total PnL = metrics[i-1].pnl -
                        // day_start_pnl
                        cumulative_offset += metrics[i - 1].pnl - day_start_pnl;
                        day_boundaries.push_back(static_cast<int>(i));
                    }
                    prev_date = date_str;
                    day_start_pnl = metrics[i].pnl;
                }
                // metrics[i].pnl is cumulative from start; subtract day_start_pnl to get relative
                // to day start, then add cumulative_offset to stack days
                full_pnl.push_back(metrics[i].pnl - day_start_pnl + cumulative_offset);
            }

            // LTTB downsample indices based on PnL path; share for Greeks
            constexpr size_t kMaxChartPoints = 1000;
            std::vector<size_t> idxs = lttb_downsample_indices(full_pnl, kMaxChartPoints);

            const size_t m = idxs.size();
            chart_pnl.reserve(m);
            chart_x_greek.reserve(m);
            chart_delta.reserve(m);
            chart_theta.reserve(m);
            chart_gamma.reserve(m);
            for (size_t k = 0; k < m; ++k) {
                const size_t idx = idxs[k];
                chart_pnl.push_back(full_pnl[idx]);
                chart_x_greek.push_back(static_cast<int>(idx));
                chart_delta.push_back(metrics[idx].delta);
                chart_theta.push_back(metrics[idx].theta);
                chart_gamma.push_back(metrics[idx].gamma);
            }
        }
        out << "\"chart_data\":{";
        out << "\"pnl\":[";
        for (size_t i = 0; i < chart_pnl.size(); ++i) {
            if (i > 0)
                out << ",";
            out << std::fixed << chart_pnl[i];
        }
        out << "],\"x_greek\":[";
        for (size_t i = 0; i < chart_x_greek.size(); ++i) {
            if (i > 0)
                out << ",";
            out << chart_x_greek[i];
        }
        out << "],\"delta\":[";
        for (size_t i = 0; i < chart_delta.size(); ++i) {
            if (i > 0)
                out << ",";
            out << chart_delta[i];
        }
        out << "],\"theta\":[";
        for (size_t i = 0; i < chart_theta.size(); ++i) {
            if (i > 0)
                out << ",";
            out << chart_theta[i];
        }
        out << "],\"gamma\":[";
        for (size_t i = 0; i < chart_gamma.size(); ++i) {
            if (i > 0)
                out << ",";
            out << chart_gamma[i];
        }
        out << "],\"day_boundaries\":[";
        for (size_t i = 0; i < day_boundaries.size(); ++i) {
            if (i > 0)
                out << ",";
            out << day_boundaries[i];
        }
        out << "]},";

        out << "\"errors\":[";
        for (size_t i = 0; i < result.errors.size(); ++i) {
            if (i > 0)
                out << ",";
            out << "\"" << json_escape(result.errors[i]) << "\"";
        }
        out << "]";
        out << "}";

        std::cout << out.str() << std::flush;
        return 0;
    } catch (const std::exception& e) {
        print_error_json(e.what());
        return 1;
    } catch (...) {
        print_error_json("Unknown C++ backtest error");
        return 1;
    }
}
