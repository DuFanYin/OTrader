#pragma once

/**
 * BacktestGateway: mock execution engine for backtest.
 *
 * Responsibility:
 *   - Given current market (via MainEngine → Portfolio), simulate order fills
 *     (LIMIT/MARKET, single-leg and combo).
 *   - Create OrderData/TradeData events and push into backtest MainEngine.
 *   - Charge fees and update cumulative_fees_ in BacktestEngine.
 *
 * This lives in infra/gateway to mirror engine_gateway_ib, but is used only by
 * backtest::BacktestEngine.
 */

#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include <string>
#include <utility>

namespace backtest {

class MainEngine;

class BacktestGateway {
  public:
    BacktestGateway(MainEngine* main_engine, double fee_rate, double slippage_bps);

    void configure_execution(double fee_rate, double slippage_bps);

    /** Simulate execution of one order:
     *  - Updates cumulative_fees
     *  - Emits Order/Trade events via MainEngine
     *  - Increments trade_counter as needed
     */
    /** Returns true if filled (trade emitted), false if still open. */
    bool execute_order(const utilities::OrderRequest& req, const std::string& orderid,
                       int& trade_counter, double& cumulative_fees);

  private:
    std::pair<double, double> get_market_bid_ask(const std::string& symbol) const;
    static double default_contract_size(const std::string& symbol);
    double calculate_order_fee(const utilities::OrderRequest& req, double fill_price) const;

    MainEngine* main_engine_;
    double fee_rate_ = 0.0;
    double slippage_bps_ = 5.0;
};

} // namespace backtest
