#pragma once

#include "strategy.hpp"

#include <array>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace strategy {

/**
 * ComboStrategy: a combo of up to four legs, opened once a day at entry_minute and closed at
 * exit_minute (all legs, or with close_itm_only those in the money, the rest left to expire), on
 * take_profit or stop_loss (x the premium), or held to expiry with hold. A combo held from a day
 * before blocks a new one. Settlement shares are sold the next morning unless hedged. Each leg is
 * a rule (right, ratio, strike as % from the forward, first expiry >= dte, or exactly dte with
 * exact_dte), so one setting runs in backtest and on paper. It enters only on a day whose day
 * before moved within prev_rv_min..prev_rv_max (realized vol of its minutes, %, the series
 * <portfolio>_minute_rv of the values by day) and when the underlying has
 * moved within move_min..move_max (% since the day's first bar). Model 1 enters only when the
 * Heston value less the executable cost (ask to buy, bid to sell) is at least min_edge USD a unit.
 */
class ComboStrategy final : public Strategy {
  public:
    struct Setting {
        Param<int> entry_minute{"entry_minute", 30, 1, 389}; // 10:00
        Param<int> exit_minute{"exit_minute", 380, 2, 390};  // 15:50
        Param<int> hold{"hold", 0, 0, 1}; // 1: held to expiry
        Param<int> quantity{"quantity", 1, 1, 100};            // combo units
        Param<double> take_profit{"take_profit", 0, 0, 100}; // x the premium; 0: none
        Param<double> stop_loss{"stop_loss", 0, 0, 100};     // x the premium; 0: none
        // Per leg: ratio (+ buy, - sell, 0 none), right (0 call, 1 put), strike % off forward, dte.
        Param<int> leg1_ratio{"leg1_ratio", 1, -10, 10};
        Param<int> leg1_right{"leg1_right", 0, 0, 1};
        Param<double> leg1_strike_pct{"leg1_strike_pct", 0, -50, 50};
        Param<int> leg1_dte{"leg1_dte", 0, 0, 365};
        Param<int> leg2_ratio{"leg2_ratio", 0, -10, 10};
        Param<int> leg2_right{"leg2_right", 0, 0, 1};
        Param<double> leg2_strike_pct{"leg2_strike_pct", 0, -50, 50};
        Param<int> leg2_dte{"leg2_dte", 0, 0, 365};
        Param<int> leg3_ratio{"leg3_ratio", 0, -10, 10};
        Param<int> leg3_right{"leg3_right", 0, 0, 1};
        Param<double> leg3_strike_pct{"leg3_strike_pct", 0, -50, 50};
        Param<int> leg3_dte{"leg3_dte", 0, 0, 365};
        Param<int> leg4_ratio{"leg4_ratio", 0, -10, 10};
        Param<int> leg4_right{"leg4_right", 0, 0, 1};
        Param<double> leg4_strike_pct{"leg4_strike_pct", 0, -50, 50};
        Param<int> leg4_dte{"leg4_dte", 0, 0, 365};
        Param<int> hedge{"hedge", 0, 0, 1};               // 1: its delta hedge keeps delta near 0
        Param<int> hedge_timer{"hedge_timer", 1, 1, 390}; // hedge every this many bars
        Param<int> delta_range{"delta_range", 20, 0, 1000}; // shares either side of zero left alone
        Param<int> model{"model", 0, 0, 1};                 // 1: enter only on a Heston edge
        Param<double> min_edge{"min_edge", 0, -1'000'000, 1'000'000}; // USD a combo unit
        Param<int> exact_dte{"exact_dte", 0, 0, 1};      // 1: no combo on a day without that expiry
        Param<int> close_itm_only{"close_itm_only", 0, 0, 1}; // 1: at exit_minute, legs in the money
        // Entry filters, % (the bounds' defaults are off): the day before's realized vol (minute
        // log returns' squares summed, square root), and the move since the day's first bar.
        Param<double> prev_rv_min{"prev_rv_min", 0, 0, 100};
        Param<double> prev_rv_max{"prev_rv_max", 100, 0, 100};
        Param<double> move_min{"move_min", 0, 0, 100};
        Param<double> move_max{"move_max", 100, 0, 100};
    };
    using Strategy::Strategy;

    void on_init() override;
    void on_day() override;
    void on_stop() override;
    void on_timer() override;

    static constexpr int kMaxLegs = 4;

  private:
    struct LegRule {
        int ratio = 0; // contracts per combo unit: bought +, sold -; 0: no leg
        model::OptionType right = model::OptionType::CALL;
        double strike_pct = 0.0; // strike: forward x (1 + strike_pct / 100), the nearest listed
        int dte = 0;
    };

    static std::array<LegRule, kMaxLegs> rules_of(const Setting& s);
    /** Each leg's option now; empty when a leg has no chain, strike or two-sided quote. */
    std::vector<model::Option> pick_options() const;
    /** The edge under Heston, USD a combo unit (NaN when it cannot be computed). */
    double heston_edge(std::span<const model::Option> options) const;
    void enter(int minute);
    /** pnl_at_entry_ for a combo held when it started: the holding's P&L less its options'
     * unrealized; false until they are valued. */
    bool resume_pnl_at_entry();
    /** Each new minute: the underlying's first price and the sum of squared log returns today. */
    void track_underlying(int minute);
    /** Whether the entry filters let a combo open now; one that rules today out says why and
     *  ends the day's tries. */
    bool filters_allow(int minute);
    /** The legs in the money now, closed; the others left to expire. */
    void close_in_the_money(int minute);

    const Setting setting_ = read_setting<Setting>();
    const std::array<LegRule, kMaxLegs> rules_ = rules_of(setting_);
    std::array<std::string, kMaxLegs> chains_{}; // each leg's chain today
    bool entered_ = false;
    bool done_ = false;
    bool sell_delivered_ = false; // shares a settlement delivered, sold at the day's first bar
    double premium_ = 0.0; // |net premium| at entry, USD: what take_profit and stop_loss scale
    double pnl_at_entry_ = 0.0; // the holding's pnl before it: earlier days' are not this combo's
    int last_minute_ = -1;
    double first_price_ = 0.0; // today's first underlying price
    double last_price_ = 0.0;
    double prev_rv_ = std::numeric_limits<double>::quiet_NaN(); // the day before's, %
};

} // namespace strategy
