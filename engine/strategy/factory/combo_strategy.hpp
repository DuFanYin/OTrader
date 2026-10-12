#pragma once

#include "template.hpp"

#include <array>
#include <chrono>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace strategy {

/**
 * ComboStrategy: a combo of up to four legs, opened once a day at entry_minute and closed at
 * exit_minute (or once its P&L reaches take_profit, or loses stop_loss, times the premium). Each
 * leg is a rule, not a contract: call or put, signed contracts per combo unit, its strike as a
 * percent from its expiry's forward (the nearest listed), its expiry the first at least dte days
 * out. The research tool backtest_combo writes a combo picked in the research views (Screener, Multi-Leg) as
 * these rules; the same setting runs in backtest and on the paper account.
 *
 * Optional: the HedgeEngine keeps its delta within delta_range shares (hedge, hedge_timer,
 * delta_range, as DeltaHedgedStraddle); and model 1 opens it only when its edge, the value under
 * Heston calibrated to each leg's chain at entry (quant::heston_calibrate) less its cost at the
 * executable prices (buy at the ask, sell at the bid), is at least min_edge USD a combo unit: the
 * screener's edge with model_vol heston.
 */
class ComboStrategy final : public OptionStrategyTemplate {
  public:
    ComboStrategy(StrategyContext* context, const std::string& strategy_name,
                  const std::string& portfolio_name,
                  const std::unordered_map<std::string, double>& setting);

    static std::span<const StrategyParam> parameters();

    void on_init_logic() override;
    void on_stop_logic() override;
    void on_timer_logic() override;

    static constexpr int kMaxLegs = 4;

  private:
    struct LegRule {
        int ratio = 0; // contracts per combo unit: bought +, sold -; 0: no leg
        model::OptionType right = model::OptionType::CALL;
        double strike_pct = 0.0; // strike: forward x (1 + strike_pct / 100), the nearest listed
        int dte = 0;
    };

    /** A new day: each leg's chain (subscribed), nothing entered. */
    void start_day(std::chrono::year_month_day day);
    /** Each leg's option now; empty when a leg has no chain, strike or two-sided quote. */
    std::vector<const model::OptionData*> pick_options() const;
    /** The edge under Heston, USD a combo unit (NaN when it cannot be computed). */
    double heston_edge(std::span<const model::OptionData* const> options) const;
    void enter(int minute);
    [[nodiscard]] bool holds_options() const;

    std::array<LegRule, kMaxLegs> rules_{};
    int entry_minute_;
    int exit_minute_;
    int quantity_;
    double take_profit_;
    double stop_loss_;
    bool hedge_;
    int hedge_timer_;
    int delta_range_;
    int model_;
    double min_edge_;

    std::chrono::year_month_day day_{};
    std::array<std::string, kMaxLegs> chains_{}; // each leg's chain today
    bool entered_ = false;
    bool done_ = false;
    double premium_ = 0.0; // |net premium| at entry, USD: what take_profit and stop_loss scale
    double pnl_at_entry_ = 0.0; // the holding's pnl before it: earlier days' are not this combo's
};

} // namespace strategy
