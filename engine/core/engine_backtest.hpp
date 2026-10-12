#pragma once

#include "adapters/backtest/backtest_types.hpp"
#include "adapters/backtest/engine_data_historical.hpp"
#include "adapters/backtest/engine_gateway_backtest.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "core/engine_main.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace core::backtest {

struct BacktestResult {
    std::string strategy_name;
    std::string portfolio_name;
    adapters::Timestamp start_time{};
    adapters::Timestamp end_time{};
    int total_timesteps = 0;
    int processed_timesteps = 0;
    double final_pnl = 0.0;
    int total_orders = 0;
    double max_delta = 0.0;
    double max_gamma = 0.0;
    double max_theta = 0.0;
    double max_drawdown = 0.0;
    int64_t total_frames = 0;
    int64_t total_rows = 0;
    std::vector<std::string> errors;
    std::vector<model::TradeData> trades; ///< contract fills (a combo's are its legs'), by time
};

struct Metric {
    int timestep = 0;
    std::string timestamp;
    double pnl = 0.0;
    double delta = 0.0;
    double theta = 0.0;
    double gamma = 0.0;
    double fees = 0.0;
};

struct DailyResult {
    std::string file_path;
    BacktestResult result;
    double daily_pnl = 0.0;
    double daily_fees = 0.0;
    size_t file_index = 0;
    std::vector<Metric> file_metrics;
    model::PortfolioSummary summary; ///< the strategy's holding at the day's end (pnl_* by source)
    bool load_failed = false; ///< Parquet could not be loaded; reason in result.errors
};

struct BacktestRunSummary {
    std::vector<Metric> metrics;
    std::vector<DailyResult> daily_results;
    std::vector<double> daily_returns;
    BacktestResult aggregated_result;
    double duration_seconds = 0.0;
    long long duration_ms = 0;
};

/**
 * The backtest assembly of core::Engine, and its run: events processed as they are put
 * (EventEngine not started), the data's clock (the latest snapshot's time), BacktestGateway as
 * the venue (orders fill at the next bar's quotes), the storage in memory, and a day's quotes
 * from Parquet (load_backtest_data). run() replays the day bar by bar: the snapshot, the orders
 * sent at the bar before filled at its quotes, then the timer (the strategy runs).
 */
class MainEngine : public core::Engine {
  public:
    using TimestepCallback = std::function<void(int timestep, adapters::Timestamp)>;

    MainEngine();
    ~MainEngine();

    /** The venue: orders wait for the next bar (run() fills them). */
    adapters::BacktestGateway* backtest_gateway() { return backtest_gateway_; }
    /** The fee per contract, and a market order's slippage; throws for a negative fee. */
    void configure_execution(double fee_rate, double slippage_bps = 5.0);

    adapters::BacktestDataEngine* load_backtest_data(const std::string& parquet_path,
                                                     const std::string& underlying_symbol = "");
    adapters::BacktestDataEngine* get_data_engine() const { return data_engine_.get(); }

    /** Called after each bar, with its number (from 1) and its time. */
    void register_timestep_callback(TimestepCallback cb);
    /** Replay the day loaded through the one strategy added (add_strategy on the day's portfolio,
     * BacktestDataEngine::portfolio_name), inited and started first. */
    BacktestResult run();

  private:
    adapters::BacktestGateway* backtest_gateway_ = nullptr; // gateway_, as what it is
    std::unique_ptr<adapters::BacktestDataEngine> data_engine_;
    std::vector<TimestepCallback> timestep_callbacks_;
};

/** Each day in parquet_files on an engine of its own (four at a time), and the days together.
 * risk_setting: risk limits in place of the defaults (RiskEngine::set; throws on a bad one). */
BacktestRunSummary
run_backtest_multi(const std::vector<std::string>& parquet_files, const std::string& strategy_name,
                   double fee_rate, double slippage_bps, double risk_free_rate,
                   const std::string& iv_price_mode, utilities::LogLevel log_level,
                   const std::unordered_map<std::string, double>& strategy_setting,
                   const std::unordered_map<std::string, double>& risk_setting = {});

} // namespace core::backtest
