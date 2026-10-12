#pragma once

/** PositionEngine: holdings, order/trade, metrics; caller passes get_portfolio. */

#include "utilities/base_engine.hpp"
#include "model/constant.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/utility.hpp"
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace model {
class PortfolioStructure;
} // namespace model

namespace core {

/** A contract held here or at the broker: the holdings summed by contract against the broker's
 * quantity (shares, and options in contracts). */
struct PositionCheck {
    std::string symbol;
    double engine = 0.0;
    double broker = 0.0;
    /** Held here, or a contract the portfolios trade: the two must agree. Else the broker's
     * alone (traded by hand, say), listed apart. */
    bool traded_here = false;

    /** Traded here, and the two quantities are not the same. */
    [[nodiscard]] bool is_mismatch() const;
};

class PositionEngine : public utilities::BaseEngine {
  public:
    /** An order filled at once, and its fills: what settling a position (closing it, or
     * delivering its shares) books. */
    struct Settlement {
        model::OrderData order;
        std::vector<model::TradeData> fills;
    };

    PositionEngine() = default;
    explicit PositionEngine(utilities::Logger* logger) : BaseEngine(logger, "Position") {}

    /** Book a fill (once; Engine::book_trade drops a repeated tradeid). A fill is one contract's
     * (adapters::Gateway); `order` is its order as sent. A combo order's fill updates that leg of
     * the combo position (get_or_create_combo_position) and the combo's quantity is recomputed
     * from its legs; without an order the fill is booked as a single contract. The result does
     * not depend on the order fills arrive in. */
    void process_trade_event(const std::string& strategy_name, const model::TradeData& trade,
                             const model::OrderData* order);

    void get_create_strategy_holding(const std::string& strategy_name);
    void remove_strategy_holding(const std::string& strategy_name);
    model::StrategyHolding& get_holding(const std::string& strategy_name);
    /** The strategy's holding; null when it has none. */
    const model::StrategyHolding* find_holding(const std::string& strategy_name) const;

    /** Every contract held here or at the broker (broker: contract → signed quantity), sorted;
     * one the portfolios trade (PortfolioStructure::trades_contract) must agree. */
    std::vector<PositionCheck> check_positions(const std::map<std::string, double>& broker,
                                               const model::PortfolioStructure& portfolios) const;
    /** The options held, by every strategy, sorted. */
    std::vector<std::string> held_options() const;
    /** The strategy's options held past their expiry by now, sorted. */
    std::vector<std::string> expired_options(const std::string& strategy_name,
                                             model::DateTime now) const;
    /** Close out the strategy's options expiring on `expiry`, the portfolio's underlying at
     * `price` at that close: in cash at intrinsic value; or, its options settling in shares, at
     * nothing, those in the money by a cent or more (OCC's exercise by exception) delivering
     * their shares at the strike. Each close-out (a single option, a combo with its legs, a
     * delivery) is an order filled at once, reference "Settlement", to book: the closes first,
     * then the deliveries. */
    std::vector<Settlement> settle_expiry(const std::string& strategy_name,
                                          std::chrono::year_month_day expiry, double price,
                                          const model::PortfolioData& portfolio) const;

    // ── The holdings against the broker's positions, as last checked

    /** Record a check against the broker's positions (check_positions): they agree when no
     * contract traded here differs. Logs it when it changed. */
    void record_broker_check(std::span<const PositionCheck> checks);
    /** Record that the broker's positions could not be had, and why. */
    void record_broker_check_failed(const std::string& reason);
    /** The last check: "not yet", "agree", "disagree" or "failed", and what was wrong. */
    std::pair<std::string, std::string> broker_check() const;
    /** Why the strategy may not start at `now`, if anything: it holds options past their expiry
     * not settled; or, broker_agreement_required, the last check against the broker's positions
     * did not agree. */
    std::optional<std::string> start_refusal(const std::string& strategy_name,
                                             model::DateTime now,
                                             bool broker_agreement_required) const;

    /** The strategy's holding valued at the portfolio's prices now (each timer tick); nothing
     * when it has none. */
    void update_metrics(const std::string& strategy_name, const model::PortfolioData& portfolio);

  private:
    enum class BrokerCheck : std::uint8_t { NotYet, Agree, Disagree, Failed };

    static void apply_underlying_trade(model::StrategyHolding& holding,
                                       const model::TradeData& trade);
    static void apply_single_leg_option_trade(model::StrategyHolding& holding,
                                              const model::TradeData& trade);
    /** The combo position for a combo order: the one with the same legs, else the one containing
     * all of the order's legs (the smallest key if several), else a new one. */
    static model::OptionPositionData& get_or_create_combo_position(model::StrategyHolding& holding,
                                                const model::OrderData& order);
    static void apply_position_change(model::BasePosition* pos,
                                      const model::TradeData& trade);

    // What a position adds to its holding's summary (current_value, total_cost, realized_pnl and
    // the Greeks times its quantity), its own Greeks and mid set from the price it is valued at.
    static model::PortfolioSummary accumulate_position(model::BasePosition* position,
                                                       const model::OptionData* option_snapshot);
    static model::PortfolioSummary
    accumulate_position(model::BasePosition* position,
                        const model::UnderlyingData* underlying_snapshot);
    static model::PortfolioSummary accumulate_option_position(model::OptionPositionData& opt,
                                                              const model::PortfolioData& portfolio);
    static void add_totals(model::PortfolioSummary& totals, const model::PortfolioSummary& metrics);
    /** What the holding's shares are in: the hedge instrument when traded, else the underlying. */
    static const model::UnderlyingData* shares_instrument(const model::StrategyHolding& holding,
                                                          const model::PortfolioData& portfolio);

    /** A holding as it stood at its last update: where the next update's attribution starts. */
    struct AttributionStart {
        model::DateTime time{};
        double pnl = 0.0;
        double underlying_price = 0.0;
        model::BasePosition shares;
        std::vector<model::BasePosition> options; // each option held: its mid, Greeks per contract
        std::vector<double> option_ivs;           // and its implied vol
    };
    /** Add the moves since the last update to the summary's pnl_* fields, then note this one. */
    void attribute_pnl(const std::string& strategy_name, model::StrategyHolding& holding,
                       const model::PortfolioData& portfolio);

    std::unordered_map<std::string, model::StrategyHolding> strategy_holdings_;
    std::unordered_map<std::string, AttributionStart> attribution_starts_;
    BrokerCheck broker_check_ = BrokerCheck::NotYet;
    std::string broker_check_problem_;
};

} // namespace core
