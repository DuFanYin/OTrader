#include "straddle_inventory_scalper.hpp"
#include "engine_option_strategy.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace strategy_cpp {

StraddleInventoryScalperStrategy::StraddleInventoryScalperStrategy(
    core::OptionStrategyEngine* strategy_engine, const std::string& strategy_name,
    const std::string& portfolio_name, const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(strategy_engine, strategy_name, portfolio_name, setting),
      trade_start_minute_(15), trade_end_minute_(300), move_atr_ratio_(0.6),
      max_straddle_spread_(2.0), profit_target_pct_(10.0), profit_target_short_pct_(6.0),
      profit_target_short_min_(3), profit_target_short_max_(8), time_stop_minutes_(12),
      loss_stop_pct_(12.0), cooldown_minutes_(3), max_daily_trades_(30) {
    auto apply = [&](const std::string& key, int& val) {
        if (auto it = setting.find(key); it != setting.end()) {
            val = static_cast<int>(it->second);
        }
    };
    auto apply_d = [&](const std::string& key, double& val) {
        if (auto it = setting.find(key); it != setting.end()) {
            val = it->second;
        }
    };
    apply("trade_start_minute", trade_start_minute_);
    apply("trade_end_minute", trade_end_minute_);
    apply_d("move_atr_ratio", move_atr_ratio_);
    apply_d("max_straddle_spread", max_straddle_spread_);
    apply_d("profit_target_pct", profit_target_pct_);
    apply_d("profit_target_short_pct", profit_target_short_pct_);
    apply("profit_target_short_min", profit_target_short_min_);
    apply("profit_target_short_max", profit_target_short_max_);
    apply("time_stop_minutes", time_stop_minutes_);
    apply_d("loss_stop_pct", loss_stop_pct_);
    apply("cooldown_minutes", cooldown_minutes_);
    apply("max_daily_trades", max_daily_trades_);
}

void StraddleInventoryScalperStrategy::on_init_logic() {
    auto chains = portfolio()->get_chain_by_expiry(0, 0);
    if (chains.empty()) {
        set_error("No 0DTE chains found");
        return;
    }
    chain_symbols_ = {chains.front()};
    subscribe_chains(chain_symbols_);
    write_log("StraddleInventoryScalper initialized on 0DTE chain: " + chains.front());
}

void StraddleInventoryScalperStrategy::on_stop_logic() {
    close_all_strategy_positions();
    write_log("Strategy stopped. Total trades: " + std::to_string(trade_count_));
}

void StraddleInventoryScalperStrategy::update_underlying_history() {
    auto* u = underlying();
    if (u == nullptr) {
        return;
    }
    double mid = u->mid_price;
    if (mid <= 0.0) {
        return;
    }
    underlying_mid_history_.push_back(mid);
    while (underlying_mid_history_.size() > kAtrPeriod + 1) {
        underlying_mid_history_.pop_front();
    }
}

double StraddleInventoryScalperStrategy::atr_10m() const {
    if (underlying_mid_history_.size() < 2) {
        return 0.0;
    }
    size_t n = std::min(underlying_mid_history_.size() - 1, kAtrPeriod);
    double sum = 0.0;
    for (size_t i = underlying_mid_history_.size() - n; i < underlying_mid_history_.size(); ++i) {
        sum += std::fabs(underlying_mid_history_[i] - underlying_mid_history_[i - 1]);
    }
    return n > 0 ? sum / static_cast<double>(n) : 0.0;
}

double StraddleInventoryScalperStrategy::move_3m() const {
    if (underlying_mid_history_.size() <= kMoveLookback) {
        return 0.0;
    }
    size_t idx = underlying_mid_history_.size() - 1;
    return std::fabs(underlying_mid_history_[idx] - underlying_mid_history_[idx - kMoveLookback]);
}

bool StraddleInventoryScalperStrategy::in_time_window() const {
    return minutes_elapsed_ >= trade_start_minute_ && minutes_elapsed_ <= trade_end_minute_;
}

std::optional<std::pair<utilities::OptionData*, utilities::OptionData*>>
StraddleInventoryScalperStrategy::get_atm_call_put_or_null() const {
    if (chain_symbols_.empty()) {
        return std::nullopt;
    }
    utilities::ChainData* chain = get_chain(chain_symbols_.front());
    if (chain == nullptr) {
        return std::nullopt;
    }
    chain->calculate_atm_price();
    if (chain->atm_index.empty()) {
        return std::nullopt;
    }
    auto c_it = chain->calls.find(chain->atm_index);
    auto p_it = chain->puts.find(chain->atm_index);
    if (c_it == chain->calls.end() || p_it == chain->puts.end()) {
        return std::nullopt;
    }
    utilities::OptionData* call = c_it->second;
    utilities::OptionData* put = p_it->second;
    if (call == nullptr || put == nullptr) {
        return std::nullopt;
    }
    return std::make_pair(call, put);
}

bool StraddleInventoryScalperStrategy::liquidity_ok(utilities::OptionData* call,
                                                    utilities::OptionData* put) const {
    if (call == nullptr || put == nullptr) {
        return false;
    }
    double spread = (call->ask_price - call->bid_price) + (put->ask_price - put->bid_price);
    return spread <= max_straddle_spread_;
}

void StraddleInventoryScalperStrategy::enter_straddle(utilities::OptionData* call,
                                                      utilities::OptionData* put) {
    if (call == nullptr || put == nullptr) {
        return;
    }
    if (call->mid_price <= 0.0 || put->mid_price <= 0.0) {
        return;
    }
    std::unordered_map<std::string, utilities::OptionData*> option_data;
    option_data["call"] = call;
    option_data["put"] = put;
    auto ids = option_order(utilities::ComboType::STRADDLE, option_data, utilities::Direction::LONG,
                            0.0, 1.0, utilities::OrderType::MARKET);
    if (!ids.empty()) {
        entry_straddle_cost_ = call->mid_price + put->mid_price;
        entry_minute_ = minutes_elapsed_;
        trade_count_++;
        write_log("Entered 0DTE straddle cost=" + std::to_string(entry_straddle_cost_) +
                  " minute=" + std::to_string(minutes_elapsed_));
    }
}

void StraddleInventoryScalperStrategy::try_enter_atm_straddle() {
    auto opt = get_atm_call_put_or_null();
    if (!opt) {
        return;
    }
    auto [call, put] = *opt;
    if (!liquidity_ok(call, put)) {
        return;
    }
    enter_straddle(call, put);
}

void StraddleInventoryScalperStrategy::check_exit() {
    auto* h = holding();
    if (h == nullptr || entry_minute_ < 0 || entry_straddle_cost_ <= 0.0) {
        return;
    }
    bool has_straddle = false;
    for (const auto& [_, pos] : h->optionPositions) {
        if (pos.quantity != 0) {
            has_straddle = true;
            break;
        }
    }
    if (!has_straddle) {
        return;
    }

    auto opt = get_atm_call_put_or_null();
    if (!opt) {
        return;
    }
    auto [call, put] = *opt;
    double current_mark = call->mid_price + put->mid_price;
    double pnl_pct = (current_mark - entry_straddle_cost_) / entry_straddle_cost_ * 100.0;
    int hold_minutes = minutes_elapsed_ - entry_minute_;

    if (pnl_pct >= profit_target_pct_) {
        write_log("Exit: profit target " + std::to_string(pnl_pct) + "%");
        close_all_strategy_positions();
        entry_minute_ = -1;
        entry_straddle_cost_ = 0.0;
        last_exit_minute_ = minutes_elapsed_;
        return;
    }
    if (hold_minutes >= profit_target_short_min_ && hold_minutes <= profit_target_short_max_ &&
        pnl_pct >= profit_target_short_pct_) {
        write_log("Exit: short-window profit " + std::to_string(pnl_pct) + "% at " +
                  std::to_string(hold_minutes) + " min");
        close_all_strategy_positions();
        entry_minute_ = -1;
        entry_straddle_cost_ = 0.0;
        last_exit_minute_ = minutes_elapsed_;
        return;
    }
    if (hold_minutes >= time_stop_minutes_) {
        write_log("Exit: time stop at " + std::to_string(hold_minutes) +
                  " min, pnl=" + std::to_string(pnl_pct) + "%");
        close_all_strategy_positions();
        entry_minute_ = -1;
        entry_straddle_cost_ = 0.0;
        last_exit_minute_ = minutes_elapsed_;
        return;
    }
    if (pnl_pct <= -loss_stop_pct_) {
        write_log("Exit: loss stop " + std::to_string(pnl_pct) + "%");
        close_all_strategy_positions();
        entry_minute_ = -1;
        entry_straddle_cost_ = 0.0;
        last_exit_minute_ = minutes_elapsed_;
    }
}

void StraddleInventoryScalperStrategy::on_timer_logic() {
    if (error()) {
        return;
    }
    auto* h = holding();
    if (h == nullptr) {
        return;
    }

    minutes_elapsed_++;
    update_underlying_history();

    bool has_position = false;
    for (const auto& [_, pos] : h->optionPositions) {
        if (pos.quantity != 0) {
            has_position = true;
            break;
        }
    }

    if (has_position) {
        check_exit();
        return;
    }

    if (trade_count_ >= max_daily_trades_) {
        return;
    }
    if (minutes_elapsed_ < last_exit_minute_ + cooldown_minutes_) {
        return;
    }
    if (!in_time_window()) {
        return;
    }

    double atr = atr_10m();
    double move = move_3m();
    if (atr <= 0.0 || move < move_atr_ratio_ * atr) {
        return;
    }

    try_enter_atm_straddle();
}

} // namespace strategy_cpp
