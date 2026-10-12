#pragma once

#include "template.hpp"

#include <chrono>
#include <cmath>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace strategy {

/** 0DTE straddle inventory scalper: gamma capture, time/vol filters. */
class StraddleInventoryScalperStrategy final : public OptionStrategyTemplate {
  public:
    StraddleInventoryScalperStrategy(StrategyContext* context, const std::string& strategy_name,
                                     const std::string& portfolio_name,
                                     const std::unordered_map<std::string, double>& setting);

    static std::span<const StrategyParam> parameters();

    void on_init_logic() override;
    void on_stop_logic() override;
    void on_timer_logic() override;

  private:
    void update_underlying_history();
    double atr_10m() const;
    double move_3m() const;
    bool in_time_window() const;
    bool liquidity_ok(const model::OptionData* call, const model::OptionData* put) const;
    /** Returns ATM call and put for current chain, or nullopt if unavailable. */
    std::optional<std::pair<const model::OptionData*, const model::OptionData*>>
    get_atm_call_put_or_null() const;
    void try_enter_atm_straddle();
    void enter_straddle(const model::OptionData* call, const model::OptionData* put);
    void check_exit();
    void exit_straddle(const std::string& reason);

    std::vector<std::string> chain_symbols_;
    std::deque<double> underlying_mid_history_;
    static constexpr size_t kAtrPeriod = 10;
    static constexpr size_t kMoveLookback = 3;

    int minutes_elapsed_ = 0; // since the 09:30 New York open
    int entry_minute_ = -1;
    double entry_straddle_cost_ = 0.0;
    const model::OptionData* entry_call_ = nullptr; // legs actually held; ATM moves after entry
    const model::OptionData* entry_put_ = nullptr;
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

} // namespace strategy
