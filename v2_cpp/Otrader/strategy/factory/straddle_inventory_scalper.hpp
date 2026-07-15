#pragma once

#include "../core/engine_option_strategy.hpp"
#include "template.hpp"

#include <chrono>
#include <cmath>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace strategy_cpp {

/** 0DTE straddle inventory scalper: gamma capture, time/vol filters. */
class StraddleInventoryScalperStrategy final : public OptionStrategyTemplate {
  public:
    StraddleInventoryScalperStrategy(core::OptionStrategyEngine* strategy_engine,
                                     const std::string& strategy_name,
                                     const std::string& portfolio_name,
                                     const std::unordered_map<std::string, double>& setting);

    void on_init_logic() override;
    void on_stop_logic() override;
    void on_timer_logic() override;

  private:
    void update_underlying_history();
    double atr_10m() const;
    double move_3m() const;
    bool in_time_window() const;
    bool liquidity_ok(utilities::OptionData* call, utilities::OptionData* put) const;
    /** Returns ATM call and put for current chain, or nullopt if unavailable. */
    std::optional<std::pair<utilities::OptionData*, utilities::OptionData*>>
    get_atm_call_put_or_null() const;
    void try_enter_atm_straddle();
    void enter_straddle(utilities::OptionData* call, utilities::OptionData* put);
    void check_exit();

    std::vector<std::string> chain_symbols_;
    std::deque<double> underlying_mid_history_;
    static constexpr size_t kAtrPeriod = 10;
    static constexpr size_t kMoveLookback = 3;

    int minutes_elapsed_ = 0;
    int entry_minute_ = -1;
    double entry_straddle_cost_ = 0.0;
    int last_exit_minute_ = -999;
    int trade_count_ = 0;

    int trade_start_minute_;
    int trade_end_minute_;
    double move_atr_ratio_;
    double max_straddle_spread_;
    double profit_target_pct_;
    double profit_target_short_pct_;
    int profit_target_short_min_;
    int profit_target_short_max_;
    int time_stop_minutes_;
    double loss_stop_pct_;
    int cooldown_minutes_;
    int max_daily_trades_;
};

} // namespace strategy_cpp
