#pragma once

/**
 * BacktestGateway: the backtest's venue (the engine's execution port, as IbGateway is live). An
 * order sent waits for the next bar; each bar (execute_pending_orders) the orders that the
 * order's portfolio's quotes allow fill (LIMIT/MARKET, single-leg and combo), are charged
 * their fees, and their updates and fills go to the engine through the callbacks, as a live
 * venue's do.
 */

#include "../../model/event.hpp"
#include "../../model/object.hpp"
#include "../../model/portfolio.hpp"
#include "../../model/portfolio_structure.hpp"
#include "../engine_gateway.hpp"
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adapters {

class BacktestGateway : public Gateway {
  public:
    /** Fills at the quotes of the order's portfolio in portfolios. id_prefix starts every order and
     * fill id (<prefix>_order_1, <prefix>_trade_1): a venue that outlives one run gives each run its
     * own, so that ids never repeat. */
    BacktestGateway(const model::PortfolioStructure& portfolios, double fee_rate,
                    double slippage_bps, std::string id_prefix = "backtest");

    void configure_execution(double fee_rate, double slippage_bps);

    /** Queued for the next bar; its id. */
    std::string send_order(const model::OrderRequest& req) override;
    /** Off the queue, and reported cancelled. */
    void cancel_order(const model::CancelRequest& req) override;
    /** Fill the queued orders the current quotes allow; the rest wait for the next bar. */
    void execute_pending_orders();
    double cumulative_fees() const { return cumulative_fees_; }

  private:
    /** The portfolio the order is in: its quotes, and the time they are of. */
    const model::PortfolioData* portfolio_of(const model::OrderRequest& req) const;
    /** The order's new state to the engine; kept as the last one told while the order works. */
    void report_order(const model::OrderData& order);
    /** Fill one order if the quotes allow (its update and fill to the engine, its fee charged);
     * true when filled. */
    bool execute_order(const model::OrderRequest& req, const std::string& orderid);
    std::pair<double, double> get_market_bid_ask(const std::string& symbol) const;
    /** Each leg's fill price, bought at the ask and sold at the bid (with slippage when
     * `market`); nullopt when a leg has no quote or a bought leg has no ask. */
    std::optional<std::vector<double>> combo_leg_prices(std::span<const model::Leg> legs,
                                                        bool market) const;
    /** A market order's price after slippage: buys pay more, sells receive less. */
    double slipped(double price, model::Direction direction) const;
    /** A contract's size when it is not known: an option's 100, an underlying's 1. */
    static double default_contract_size(const std::string& symbol);
    double calculate_order_fee(const model::OrderRequest& req, double fill_price) const;

    const model::PortfolioStructure& portfolios_;
    double fee_rate_ = 0.0;
    double slippage_bps_ = 5.0;
    std::unordered_map<std::string, model::OrderRequest> open_orders_;
    std::unordered_map<std::string, model::OrderData> reported_; // the working ones, as last told
    std::string id_prefix_;
    int order_counter_ = 0;
    int trade_counter_ = 0;
    double cumulative_fees_ = 0.0;
};

} // namespace adapters
