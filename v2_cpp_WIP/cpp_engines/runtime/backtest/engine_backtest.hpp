#pragma once

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
    /** Execute one order (drain pending). */
    void execute_order_impl(const utilities::OrderRequest& req, const std::string& orderid);
    /** Run all pending orders (sent in previous timestep) with current step's market. */
    void execute_pending_orders();
    /** (bid, ask) for symbol; (0,0) if not found. */
    std::pair<double, double> get_market_bid_ask(const std::string& symbol) const;
    static double default_contract_size(const std::string& symbol);
    double calculate_order_fee(const utilities::OrderRequest& req, double fill_price) const;

    std::unique_ptr<MainEngine> main_engine_;
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

    /** Orders sent this timestep; executed at start of next timestep. */
    std::vector<std::pair<std::string, utilities::OrderRequest>> pending_orders_;
    int order_counter_ = 0;
    int trade_counter_ = 0;
};

} // namespace backtest
