#include "straddle_inventory_scalper.hpp"
#include "strategy_registry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace strategy {

namespace {

// name, default, min, max, integer
constexpr std::array kParameters{
    StrategyParam{"trade_start_minute", 15, 0, 390, true}, // minutes after the 09:30 open
    StrategyParam{"trade_end_minute", 300, 0, 390, true},
    StrategyParam{"move_atr_ratio", 0.6, 0.0, 10.0},
    StrategyParam{"max_straddle_spread", 2.0, 0.0, 100.0},
    StrategyParam{"profit_target_pct", 10.0, 0.0, 1000.0},
    StrategyParam{"profit_target_short_pct", 6.0, 0.0, 1000.0},
    StrategyParam{"profit_target_short_min", 3, 0, 390, true},
    StrategyParam{"profit_target_short_max", 8, 0, 390, true},
    StrategyParam{"time_stop_minutes", 10, 1, 390, true},
    StrategyParam{"loss_stop_pct", 10.0, 0.0, 100.0},
    StrategyParam{"cooldown_minutes", 3, 0, 390, true},
    StrategyParam{"max_daily_trades", 100, 0, 1000, true},
};

} // namespace

StraddleInventoryScalperStrategy::StraddleInventoryScalperStrategy(
    StrategyContext* context, const std::string& strategy_name, const std::string& portfolio_name,
    const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(context, strategy_name, portfolio_name, setting),
      trade_start_minute_(param<int>("trade_start_minute")),
      trade_end_minute_(param<int>("trade_end_minute")), move_atr_ratio_(param("move_atr_ratio")),
      max_straddle_spread_(param("max_straddle_spread")),
      profit_target_pct_(param("profit_target_pct")),
      profit_target_short_pct_(param("profit_target_short_pct")),
      profit_target_short_min_(param<int>("profit_target_short_min")),
      profit_target_short_max_(param<int>("profit_target_short_max")),
      time_stop_minutes_(param<int>("time_stop_minutes")), loss_stop_pct_(param("loss_stop_pct")),
      cooldown_minutes_(param<int>("cooldown_minutes")),
      max_daily_trades_(param<int>("max_daily_trades")) {}

auto StraddleInventoryScalperStrategy::parameters() -> std::span<const StrategyParam> {
    return kParameters;
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

std::optional<std::pair<const model::OptionData*, const model::OptionData*>>
StraddleInventoryScalperStrategy::get_atm_call_put_or_null() const {
    if (chain_symbols_.empty()) {
        return std::nullopt;
    }
    const model::ChainData* chain = get_chain(chain_symbols_.front());
    if (chain == nullptr) {
        return std::nullopt;
    }
    if (chain->atm_strike <= 0) {
        return std::nullopt;
    }
    auto c_it = chain->calls.find(chain->atm_strike);
    auto p_it = chain->puts.find(chain->atm_strike);
    if (c_it == chain->calls.end() || p_it == chain->puts.end()) {
        return std::nullopt;
    }
    const model::OptionData* call = c_it->second;
    const model::OptionData* put = p_it->second;
    if (call == nullptr || put == nullptr) {
        return std::nullopt;
    }
    return std::make_pair(call, put);
}

bool StraddleInventoryScalperStrategy::liquidity_ok(const model::OptionData* call,
                                                    const model::OptionData* put) const {
    if (call == nullptr || put == nullptr) {
        return false;
    }
    double spread = (call->ask_price - call->bid_price) + (put->ask_price - put->bid_price);
    return spread <= max_straddle_spread_;
}

void StraddleInventoryScalperStrategy::enter_straddle(const model::OptionData* call,
                                                      const model::OptionData* put) {
    if (call == nullptr || put == nullptr) {
        return;
    }
    if (call->mid_price <= 0.0 || put->mid_price <= 0.0) {
        return;
    }
    std::unordered_map<std::string, const model::OptionData*> option_data;
    option_data["call"] = call;
    option_data["put"] = put;
    auto ids = option_order(model::ComboType::STRADDLE, option_data, model::Direction::LONG,
                            0.0, 1.0, model::OrderType::MARKET);
    if (!ids.empty()) {
        entry_straddle_cost_ = call->mid_price + put->mid_price;
        entry_call_ = call;
        entry_put_ = put;
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
    if (h == nullptr || entry_minute_ < 0 || entry_straddle_cost_ <= 0.0 ||
        entry_call_ == nullptr || entry_put_ == nullptr) {
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

    // Mark the straddle we hold, not the current ATM one (the strike moves with the underlying).
    double current_mark = entry_call_->mid_price + entry_put_->mid_price;
    double pnl_pct = (current_mark - entry_straddle_cost_) / entry_straddle_cost_ * 100.0;
    int hold_minutes = minutes_elapsed_ - entry_minute_;

    if (pnl_pct >= profit_target_pct_) {
        exit_straddle("profit target " + std::to_string(pnl_pct) + "%");
        return;
    }
    if (hold_minutes >= profit_target_short_min_ && hold_minutes <= profit_target_short_max_ &&
        pnl_pct >= profit_target_short_pct_) {
        exit_straddle("short-window profit " + std::to_string(pnl_pct) + "% at " +
                      std::to_string(hold_minutes) + " min");
        return;
    }
    if (hold_minutes >= time_stop_minutes_) {
        exit_straddle("time stop at " + std::to_string(hold_minutes) +
                      " min, pnl=" + std::to_string(pnl_pct) + "%");
        return;
    }
    if (pnl_pct <= -loss_stop_pct_) {
        exit_straddle("loss stop " + std::to_string(pnl_pct) + "%");
    }
}

void StraddleInventoryScalperStrategy::exit_straddle(const std::string& reason) {
    write_log("Exit: " + reason);
    close_all_strategy_positions();
    entry_minute_ = -1;
    entry_straddle_cost_ = 0.0;
    entry_call_ = nullptr;
    entry_put_ = nullptr;
    last_exit_minute_ = minutes_elapsed_;
}

void StraddleInventoryScalperStrategy::on_timer_logic() {
    if (error()) {
        return;
    }
    auto* h = holding();
    if (h == nullptr) {
        return;
    }

    minutes_elapsed_ = model::minutes_since_open(now());
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

REGISTER_STRATEGY(StraddleInventoryScalperStrategy);

} // namespace strategy
