#pragma once

/**
 * The backtest's venue: orders fill from the next bar (plus delay_bars), each leg at its fill
 * position in its spread (1: buys at the ask, sells at the bid), fully (Quote) or up to the shown
 * size less the bar's earlier fills (ShownSize).
 */

#include "model/event.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/portfolio_spec.hpp"
#include "model/portfolios.hpp"
#include "core/gateway/gateway.hpp"
#include <array>
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

    /** What filling costs, and what of the quotes an order can take. Fees are IBKR's (kCommission,
     * kOptionOrderMin, kShareFee, kShareOrderMin) and an option's exchange fee (its portfolio's
     * spec, model::PortfolioSpec::exchange_fee), times fee_scale: 1 as charged, 0 none. A fill
     * position is where in its spread a leg fills: its mid plus this many half spreads toward its
     * side (1: at the bid or ask; below 1 inside the spread, above 1 beyond it), for an order of
     * one leg and for one of several. */
    struct Execution {
        double fee_scale = 0.0;
        double fill_position = 1.0;
        double multi_leg_fill_position = 1.0;
        FillModel fill = FillModel::Quote;
        int delay_bars = 0; // bars an order waits beyond the next before it can fill
    };

    /** IBKR's commission an option contract by its price (fixed pricing, exchange fees in it but
     * an index option's), an order's contracts at least kOptionOrderMin; a share kShareFee, an
     * order's shares at least kShareOrderMin. An order's least is of all its fills. */
    static constexpr std::array kCommission{model::FeeTier{.below = 0.05, .fee = 0.25},
                                            model::FeeTier{.below = 0.10, .fee = 0.50},
                                            model::FeeTier{.fee = 0.65}};
    static constexpr double kOptionOrderMin = 1.0;
    static constexpr double kShareFee = 0.005;
    static constexpr double kShareOrderMin = 1.0;

    /** id_prefix starts every order and fill id; a venue reused across runs gives each run its own
     * so ids never repeat. */
    BacktestGateway(const model::Portfolios& portfolios, Execution execution,
                    std::string id_prefix = "backtest");

    /** Throws for a negative fee scale, delay or fill position. */
    void configure_execution(Execution execution);

    std::string send_order(const model::OrderRequest& req) override;
    void cancel_order(const model::CancelRequest& req) override;
    /** In send order; the unfilled rest waits for the next bar. */
    void execute_pending_orders();
    double cumulative_fees() const { return cumulative_fees_; }

  protected:
    std::pair<double, double> get_market_bid_ask(const std::string& symbol) const;

    const model::Portfolios& portfolios_;

  private:
    struct Pending {
        std::string order_id;
        model::OrderRequest req;
        double filled = 0.0;
        int wait = 0; // bars still to wait before it can fill
        // Its fills' fees before the orders' least, and what it has been charged.
        double option_fees = 0.0;
        double share_fees = 0.0;
        double charged = 0.0;
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
    /** What a fill of `quantity` units at these leg prices adds to the order's fees: each leg's
     * contracts or shares, by its ratio, the order's least taken over all its fills. */
    double fill_fee(Pending& pending, double quantity, const std::vector<double>& prices) const;

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
