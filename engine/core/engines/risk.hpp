#pragma once

/**
 * RiskEngine: the check every order passes before it goes to the venue (Engine::send_order: a
 * strategy's, its hedge's, the flattening before the close), and the switch that halts trading.
 *
 * The limits are settings, each declared with a default and a range (parameters()); the defaults
 * leave a backtest as it was, a paper or live engine sets its own (Engine::set_risk_settings,
 * saved in the storage). The same checks run in a backtest. A refused order is logged (WARNING),
 * counted and kept (the latest kRecentRefusals) for the UI and the daily report.
 *
 * Halted (the operator's stop, Engine::halt): no order goes out but one that only reduces what a
 * strategy holds, until the operator clears it; the halt is saved, so a restart keeps it.
 *
 * Event thread: it reads the holdings, the working orders and the quotes as they stand.
 */

#include "model/object.hpp"
#include "model/portfolio_structure.hpp"
#include "strategy/strategy_param.hpp"
#include "utilities/base_engine.hpp"
#include "core/engines/execution.hpp"
#include "core/engines/position.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace core {

class RiskEngine : public utilities::BaseEngine {
  public:
    /** How many refusals are kept. */
    static constexpr std::size_t kRecentRefusals = 50;

    /** The limits, their defaults and ranges:
     *  - max_option_order / max_share_order: contracts (each leg: the order's volume times its
     *    ratio) / shares in one order;
     *  - max_option_position / max_share_position: what a strategy may hold of one contract /
     *    of shares, its working orders counted, after the order;
     *  - max_order_premium: USD an order may pay for options (0: no limit);
     *  - price_band: a limit price at most this fraction of the mid away from it, or within
     *    price_band_floor dollars (0: not checked);
     *  - market_max_spread: a market order needs a two-sided quote whose spread is at most this
     *    fraction of the mid (0: not checked);
     *  - short_options: 0 refuses an order that would leave the strategy short an option (only
     *    losses with a floor: buying);
     *  - flatten_minutes: options settled in shares (SPY's) and expiring today are closed this many
     *    minutes before the close, and none is bought after (0: off). */
    static std::span<const strategy::StrategyParam> parameters();

    RiskEngine(utilities::Logger* logger, const model::PortfolioStructure& portfolios,
               const PositionEngine& positions, const ExecutionEngine& execution);

    /** Why the order is refused; nullopt: it may go. strategy_name: whose (empty: not a
     * strategy's: its holding is none); portfolio: where its quotes are (empty: any). */
    std::optional<std::string> refusal(const std::string& strategy_name,
                                       const std::string& portfolio, const model::OrderRequest& req,
                                       model::DateTime now);
    /** refusal(); a refused order is logged, counted and kept. */
    bool allow(const std::string& strategy_name, const std::string& portfolio,
               const model::OrderRequest& req, model::DateTime now);

    /** Every limit's value. */
    [[nodiscard]] const std::unordered_map<std::string, double>& settings() const {
        return settings_;
    }
    [[nodiscard]] double setting(std::string_view name) const;
    /** Set these limits; throws std::invalid_argument on an unknown name or a value out of its
     * range (none is set then). */
    void set(const std::unordered_map<std::string, double>& values);

    void halt(const std::string& reason, model::DateTime at);
    void clear_halt();
    [[nodiscard]] bool halted() const { return halted_; }
    [[nodiscard]] const std::string& halt_reason() const { return halt_reason_; }
    [[nodiscard]] model::DateTime halted_at() const { return halted_at_; }

    /** Orders refused so far, and the latest of them ("HH:MM:SS strategy symbol: reason"). */
    [[nodiscard]] std::uint64_t refused() const { return refused_; }
    [[nodiscard]] const std::deque<std::string>& recent_refusals() const {
        return recent_refusals_;
    }

    /** Within flatten_minutes of the close of an option expiring today and settled in shares
     * (the portfolio's), at now: it is to be closed, and none of it bought. */
    [[nodiscard]] bool in_flatten_window(const std::string& option_symbol,
                                         const model::PortfolioData* portfolio,
                                         model::DateTime now) const;

  private:
    /** What the order changes, contract by contract: + bought, - sold (each leg's volume times
     * its ratio). */
    static std::vector<std::pair<std::string, double>> changes(const model::OrderRequest& req);
    /** The strategy's holding, contract by contract (signed), and what its working orders would
     * add. */
    void held_and_working(const std::string& strategy_name,
                          std::unordered_map<std::string, double>& held,
                          std::unordered_map<std::string, double>& working);
    const model::PortfolioData* portfolio_of(const std::string& portfolio,
                                             const std::string& symbol) const;
    /** bid, ask and mid of the symbol in the portfolio (an option, the underlying or the hedge
     * instrument); nullopt when it has none. */
    std::optional<std::array<double, 3>> quote(const model::PortfolioData* portfolio,
                                               const std::string& symbol) const;

    const model::PortfolioStructure& portfolios_;
    const PositionEngine& positions_;
    const ExecutionEngine& execution_;
    std::unordered_map<std::string, double> settings_;
    bool halted_ = false;
    std::string halt_reason_;
    model::DateTime halted_at_{};
    std::uint64_t refused_ = 0;
    std::deque<std::string> recent_refusals_;
};

} // namespace core
