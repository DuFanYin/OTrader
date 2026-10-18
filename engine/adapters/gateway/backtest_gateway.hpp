#pragma once

/**
 * The backtest's venue: orders fill from the next bar (plus delay_bars), each leg at its fill
 * position in its spread (1: buys at the ask, sells at the bid), fully (Quote) or up to the shown
 * size less the bar's earlier fills (ShownSize).
 */

#include "model/event.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/portfolios.hpp"
#include "core/gateway/gateway.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adapters {

class BacktestGateway : public core::Gateway {
  public:
    /** How much of an order a bar can fill. */
    enum class FillModel : std::uint8_t {
        Quote,     // all of it, whatever size is shown
        ShownSize, // an option's at most the size shown at its side (shares: all of it)
    };

    /** What filling costs, and what of the quotes an order can take. A fill position is where in
     * its spread a leg fills: its mid plus this many half spreads toward its side (1: at the bid
     * or ask; below 1 inside the spread, above 1 beyond it), for an order of one leg and for one
     * of several. */
    struct Execution {
        double fee_rate = 0.0; // USD a contract (shares: IBKR's fixed rate a share)
        double fill_position = 1.0;
        double multi_leg_fill_position = 1.0;
        FillModel fill = FillModel::Quote;
        int delay_bars = 0; // bars an order waits beyond the next before it can fill
    };

    /** id_prefix starts every order and fill id; a venue reused across runs gives each run its own
     * so ids never repeat. */
    BacktestGateway(const model::Portfolios& portfolios, Execution execution,
                    std::string id_prefix = "backtest");

    /** Throws for a negative fee, delay or fill position. */
    void configure_execution(Execution execution);

    std::string send_order(const model::OrderRequest& req) override;
    void cancel_order(const model::CancelRequest& req) override;
    /** In send order; the unfilled rest waits for the next bar. */
    void execute_pending_orders();
    double cumulative_fees() const { return cumulative_fees_; }

  protected:
    std::pair<double, double> get_market_bid_ask(const std::string& symbol) const;
    /** A contract's size when it is not known: an option's 100, an underlying's 1. */
    static double default_contract_size(const std::string& symbol);

    const model::Portfolios& portfolios_;

  private:
    struct Pending {
        std::string order_id;
        model::OrderRequest req;
        double filled = 0.0;
        int wait = 0; // bars still to wait before it can fill
    };

    const model::Portfolio* portfolio_of(const model::OrderRequest& req) const;
    void report_order(const model::Order& order);
    /** True once filled whole. */
    bool execute_order(Pending& pending);
    /** In the order's units. */
    double fillable(const model::OrderRequest& req, double remaining) const;
    /** What this bar can still fill of one contract on one side: the size shown there, less what
     * the bar's fills took; shares and an unknown contract, unlimited. */
    double shown_left(const std::string& symbol, model::Side side) const;
    /** The bar's fills take from the sizes shown: `quantity` units of each leg, by its ratio. */
    void take(const model::OrderRequest& req, double quantity);
    /** Each leg's fill price: at the order's fill position in the leg's spread;
     * nullopt when a leg has no quote, a crossed one (bid above ask), a bought leg no ask, or a
     * contract sold alone no bid. */
    std::optional<std::vector<double>> leg_prices(std::span<const model::Leg> legs) const;
    /** The fee of `quantity` units: each leg's contracts or shares, by its ratio. */
    double calculate_order_fee(const model::OrderRequest& req, double quantity) const;

    Execution execution_;
    std::vector<Pending> open_orders_; // in the order sent
    std::unordered_map<std::string, model::Order> reported_; // the working ones, as last told
    // This bar's fills, by contract: what they took of the size shown at the ask and at the bid.
    std::unordered_map<std::string, double> taken_at_ask_;
    std::unordered_map<std::string, double> taken_at_bid_;
    std::string id_prefix_;
    int order_counter_ = 0;
    int trade_counter_ = 0;
    double cumulative_fees_ = 0.0;
};

} // namespace adapters
