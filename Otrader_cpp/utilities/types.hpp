#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace backtest {

using Timestamp = std::chrono::system_clock::time_point;

struct OptionSnapshot {
    std::string symbol;
    std::optional<double> bid_px;
    std::optional<double> ask_px;
    std::optional<int> bid_sz;
    std::optional<int> ask_sz;

    [[nodiscard]] std::optional<double> mid() const {
        if (bid_px && ask_px) return (*bid_px + *ask_px) / 2.0;
        if (bid_px) return *bid_px;
        if (ask_px) return *ask_px;
        return std::nullopt;
    }
};

struct UnderlyingSnapshot {
    std::optional<double> bid_px;
    std::optional<double> ask_px;
    std::optional<int> bid_sz;
    std::optional<int> ask_sz;

    [[nodiscard]] std::optional<double> mid() const {
        if (bid_px && ask_px) return (*bid_px + *ask_px) / 2.0;
        if (bid_px) return *bid_px;
        if (ask_px) return *ask_px;
        return std::nullopt;
    }
};

struct BacktestPortfolio {
    UnderlyingSnapshot underlying;
    std::unordered_map<std::string, OptionSnapshot> options;
};

struct BacktestResult {
    std::string strategy_name;
    std::string portfolio_name;
    Timestamp start_time{};
    Timestamp end_time{};
    int total_timesteps = 0;
    int processed_timesteps = 0;
    double final_pnl = 0.0;
    int total_orders = 0;  // 订单数量（不再区分 trade 与 win/lose）
    double max_delta = 0.0;
    double max_gamma = 0.0;
    double max_theta = 0.0;
    double max_drawdown = 0.0;  // Maximum drawdown (peak to trough)
    // Counters for data volume
    int64_t total_frames = 0;  // number of timestep frames processed
    int64_t total_rows = 0;    // total number of rows processed (columnar frames)
    std::vector<std::string> errors;
};

struct DataMeta {
    std::string path;
    int64_t row_count = 0;
    std::string time_column;
    std::string ts_start;
    std::string ts_end;
};

}  // namespace backtest
