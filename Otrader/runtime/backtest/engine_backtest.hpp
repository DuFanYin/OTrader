#pragma once

#include "../../infra/gateway/engine_gateway_backtest.hpp"
#include "constant.hpp"
#include "engine_data_historical.hpp"
#include "engine_main.hpp"
#include "object.hpp"
#include "types.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace backtest {

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
};

struct BacktestRunSummary {
    std::vector<Metric> metrics;
    std::vector<DailyResult> daily_results;
    std::vector<double> daily_returns;
    BacktestResult aggregated_result;
    double duration_seconds = 0.0;
    long long duration_ms = 0;
};

class BacktestEngine {
  public:
    using TimestepCallback = std::function<void(int timestep, Timestamp)>;

    BacktestEngine();

    void load_backtest_data(std::string const& parquet_path,
                            std::string const& underlying_symbol = "");

    void add_strategy(std::string const& strategy_name,
                      std::unordered_map<std::string, double> const& setting = {});

    void register_timestep_callback(TimestepCallback cb);
    void configure_execution(double fee_rate, double slippage_bps = 5.0);
    double get_cumulative_fees() const { return cumulative_fees_; }

    BacktestResult run();

    std::unordered_map<std::string, double> get_current_state() const;

    MainEngine* main_engine() { return main_engine_.get(); }
    const MainEngine* main_engine() const { return main_engine_.get(); }
    BacktestDataEngine* data_engine() {
        return main_engine_ ? main_engine_->get_data_engine() : nullptr;
    }

    // Reset state (clear data/strategy/metrics)
    void reset();

    void close();

  private:
    /** Queue order for next timestep; returns orderid. */
    std::string submit_order(const utilities::OrderRequest& req);
    /** Run all pending orders (sent in previous timestep) with current step's market. */
    void execute_pending_orders();
    /** Execute one order via BacktestGateway; returns filled. */
    bool execute_order_impl(const utilities::OrderRequest& req, const std::string& orderid);

    std::unique_ptr<MainEngine> main_engine_;
    BacktestGateway gateway_;
    std::string strategy_name_;
    std::unordered_map<std::string, double> strategy_setting_;
    std::vector<TimestepCallback> timestep_callbacks_;
    int current_timestep_ = 0;
    double current_pnl_ = 0.0;
    double current_delta_ = 0.0;
    double max_delta_ = 0.0;
    double max_gamma_ = 0.0;
    double max_theta_ = 0.0;
    double peak_pnl_ = 0.0;
    double max_drawdown_ = 0.0;
    int total_orders_ = 0;
    std::vector<std::string> errors_;
    double fee_rate_ = 0.0;
    double slippage_bps_ = 5.0;
    double cumulative_fees_ = 0.0;

    /** Open orders (including those created this timestep); executed at start of next timestep. */
    std::unordered_map<std::string, utilities::OrderRequest> open_orders_;
    int order_counter_ = 0;
    int trade_counter_ = 0;
};

BacktestRunSummary
run_backtest_multi(const std::vector<std::string>& parquet_files, const std::string& strategy_name,
                   double fee_rate, double slippage_bps, double risk_free_rate,
                   const std::string& iv_price_mode, int log_level,
                   const std::unordered_map<std::string, double>& strategy_setting);

} // namespace backtest
