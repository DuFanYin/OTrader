#include "zero_dte_buyers.hpp"
#include "strategy_registry.hpp"

#include "../../model/utility.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <random>

namespace strategy {

namespace {

// name, default, min, max, integer. Shared by the three: what to buy, how much, when to sell.
constexpr StrategyParam kBudget{"premium_budget", 1500, 1, 1'000'000, false}; // USD per trade
constexpr StrategyParam kOtmSteps{"otm_steps", 0, 0, 20, true};     // strikes out of the money
constexpr StrategyParam kExitMinute{"exit_minute", 380, 1, 390, true}; // 15:50

constexpr std::array kMomentumParameters{
    kBudget,
    kOtmSteps,
    kExitMinute,
    StrategyParam{"signal_minutes", 30, 1, 120, true}, // the move measured: open to 10:00
    StrategyParam{"entry_minute", 360, 30, 389, true}, // 15:30
    StrategyParam{"min_move_bp", 0, 0, 500, false},    // a smaller move: no trade that day
};

constexpr std::array kBreakoutParameters{
    kBudget,
    kOtmSteps,
    kExitMinute,
    StrategyParam{"range_minutes", 30, 5, 120, true},
    StrategyParam{"last_entry_minute", 300, 6, 389, true}, // 14:30: no breakout after
};

constexpr std::array kRandomParameters{
    kBudget,
    kOtmSteps,
    kExitMinute,
    StrategyParam{"seed", 1, 0, 1'000'000, true},
    StrategyParam{"earliest_minute", 30, 1, 389, true},
    StrategyParam{"latest_minute", 330, 1, 389, true},
    StrategyParam{"hold_minutes", 30, 1, 389, true},
};

auto side_name(model::OptionType side) -> const char* {
    return side == model::OptionType::CALL ? "call" : "put";
}

} // namespace

// ── ZeroDteBuyer

ZeroDteBuyer::ZeroDteBuyer(StrategyContext* context, const std::string& strategy_name,
                           const std::string& portfolio_name,
                           const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(context, strategy_name, portfolio_name, setting),
      premium_budget_(param("premium_budget")), otm_steps_(param<int>("otm_steps")),
      exit_minute_(param<int>("exit_minute")) {}

void ZeroDteBuyer::on_init_logic() {
    write_log(std::format("Buys one 0DTE option a day for up to {:.0f} USD, sold by minute {}",
                          premium_budget_, exit_minute_));
}

void ZeroDteBuyer::on_stop_logic() {}

void ZeroDteBuyer::start_day(std::chrono::year_month_day day) {
    day_ = day;
    price_ = open_price_ = high_ = low_ = 0.0;
    first_minute_ = -1;
    entered_minute_ = -1;
    done_ = false;
    chain_symbol_.clear();
    const std::vector<std::string> chains = portfolio()->get_chain_by_expiry(0, 0);
    if (chains.empty()) {
        write_log(std::format("{}: no chain expires today: no trade", std::format("{}", day)),
                  utilities::WARNING);
        done_ = true;
        return;
    }
    chain_symbol_ = chains.front();
    subscribe_chains(std::vector{chain_symbol_});
}

auto ZeroDteBuyer::holds_options() const -> bool {
    if (holding() == nullptr) {
        return false;
    }
    bool held = false;
    holding()->for_each_open_option([&held](const model::BasePosition&) { held = true; });
    return held;
}

void ZeroDteBuyer::on_timer_logic() {
    const model::DateTime t = now();
    const std::chrono::year_month_day today = model::new_york_date(t);
    if (today != day_) {
        start_day(today);
    }
    const int minute = model::minutes_since_open(t);
    const model::UnderlyingData* u = underlying();
    if (u == nullptr || minute < 1 || minute > 390) {
        return;
    }
    const double mid = u->bid_price > 0.0 && u->ask_price > 0.0
                           ? 0.5 * (u->bid_price + u->ask_price)
                           : u->mid_price;
    if (mid <= 0.0) {
        return;
    }
    price_ = mid;
    if (open_price_ <= 0.0) {
        open_price_ = high_ = low_ = mid;
        first_minute_ = minute;
    }
    high_ = std::max(high_, mid);
    low_ = std::min(low_, mid);

    if (entered_minute_ >= 0 && !done_) {
        if (exit_due(minute) && holds_options() && !has_active_order()) {
            write_log(std::format("Minute {}: selling what it bought at minute {}", minute,
                                  entered_minute_));
            close_all_strategy_positions();
            done_ = true;
        }
        return;
    }
    if (done_ || has_active_order() || minute >= exit_minute_) {
        return;
    }
    if (const std::optional<model::OptionType> side = entry_signal(minute)) {
        buy(*side, minute);
    }
}

void ZeroDteBuyer::buy(model::OptionType side, int minute) {
    const model::ChainData* chain = get_chain(chain_symbol_);
    if (chain == nullptr || chain->atm_strike <= 0) {
        return; // no quotes to center on yet: the next bar
    }
    // At the money, then otm_steps strikes away from it: up for a call, down for a put.
    const auto& strikes = chain->strikes;
    const auto atm = std::ranges::find(strikes, chain->atm_strike);
    if (atm == strikes.end()) {
        return;
    }
    const auto at = atm - strikes.begin();
    const auto pick = std::clamp<std::ptrdiff_t>(
        side == model::OptionType::CALL ? at + otm_steps_ : at - otm_steps_, 0,
        static_cast<std::ptrdiff_t>(strikes.size()) - 1);
    const auto& options = side == model::OptionType::CALL ? chain->calls : chain->puts;
    const auto it = options.find(strikes[static_cast<std::size_t>(pick)]);
    if (it == options.end() || it->second == nullptr || it->second->ask_price <= 0.0) {
        return;
    }
    const model::OptionData* option = it->second;
    const double size = option->size > 0.0 ? option->size : 100.0;
    const double contracts = std::floor(premium_budget_ / (option->ask_price * size));
    if (contracts < 1.0) {
        write_log(std::format("Minute {}: one {} costs {:.0f} USD, over the budget: no trade",
                              minute, option->symbol, option->ask_price * size),
                  utilities::WARNING);
        done_ = true;
        return;
    }
    const std::vector<std::string> ids = option_order(
        model::ComboType::SINGLE_LEG, {{option->symbol, option}}, model::Direction::LONG, 0.0,
        contracts, model::OrderType::MARKET);
    if (ids.empty()) {
        done_ = true; // refused (the risk check says why): no second try today
        return;
    }
    entered_minute_ = minute;
    write_log(std::format("Minute {}: buys {} {} {} at about {:.2f} (underlying {:.2f}, open "
                          "{:.2f})",
                          minute, contracts, side_name(side), option->symbol, option->ask_price,
                          price_, open_price_));
}

// ── LateDayMomentumStrategy

LateDayMomentumStrategy::LateDayMomentumStrategy(
    StrategyContext* context, const std::string& strategy_name, const std::string& portfolio_name,
    const std::unordered_map<std::string, double>& setting)
    : ZeroDteBuyer(context, strategy_name, portfolio_name, setting),
      signal_minutes_(param<int>("signal_minutes")), entry_minute_(param<int>("entry_minute")),
      min_move_bp_(param("min_move_bp")) {}

auto LateDayMomentumStrategy::parameters() -> std::span<const StrategyParam> {
    return kMomentumParameters;
}

auto LateDayMomentumStrategy::entry_signal(int minute) -> std::optional<model::OptionType> {
    if (signal_day_ != day()) {
        signal_day_ = day();
        signal_ = 0.0;
    }
    if (minute <= signal_minutes_ || !saw_open()) {
        return std::nullopt;
    }
    if (signal_ == 0.0 && open_price() > 0.0) {
        signal_ = price() / open_price() - 1.0; // first bar past the window: close enough
    }
    if (minute < entry_minute_ || signal_ == 0.0 ||
        std::abs(signal_) * 10'000.0 < min_move_bp_) {
        return std::nullopt;
    }
    return signal_ > 0.0 ? model::OptionType::CALL : model::OptionType::PUT;
}

// ── OpeningRangeBreakoutStrategy

OpeningRangeBreakoutStrategy::OpeningRangeBreakoutStrategy(
    StrategyContext* context, const std::string& strategy_name, const std::string& portfolio_name,
    const std::unordered_map<std::string, double>& setting)
    : ZeroDteBuyer(context, strategy_name, portfolio_name, setting),
      range_minutes_(param<int>("range_minutes")),
      last_entry_minute_(param<int>("last_entry_minute")) {}

auto OpeningRangeBreakoutStrategy::parameters() -> std::span<const StrategyParam> {
    return kBreakoutParameters;
}

auto OpeningRangeBreakoutStrategy::entry_signal(int minute) -> std::optional<model::OptionType> {
    if (minute <= range_minutes_) {
        range_high_ = high(); // the range so far
        range_low_ = low();
        return std::nullopt;
    }
    if (minute > last_entry_minute_ || range_high_ <= 0.0 || !saw_open()) {
        return std::nullopt;
    }
    if (price() > range_high_) {
        return model::OptionType::CALL;
    }
    if (price() < range_low_) {
        return model::OptionType::PUT;
    }
    return std::nullopt;
}

// ── RandomBuyStrategy

RandomBuyStrategy::RandomBuyStrategy(StrategyContext* context, const std::string& strategy_name,
                                     const std::string& portfolio_name,
                                     const std::unordered_map<std::string, double>& setting)
    : ZeroDteBuyer(context, strategy_name, portfolio_name, setting), seed_(param<int>("seed")),
      earliest_minute_(param<int>("earliest_minute")),
      latest_minute_(std::max(param<int>("latest_minute"), param<int>("earliest_minute"))),
      hold_minutes_(param<int>("hold_minutes")) {}

auto RandomBuyStrategy::parameters() -> std::span<const StrategyParam> { return kRandomParameters; }

void RandomBuyStrategy::draw(std::chrono::year_month_day day) {
    // The same seed and day give the same draw, in a backtest and live.
    const auto days = std::chrono::sys_days{day}.time_since_epoch().count();
    std::mt19937_64 rng(static_cast<std::uint64_t>(seed_) * 1'000'003ULL +
                        static_cast<std::uint64_t>(days));
    draw_minute_ = std::uniform_int_distribution<int>(earliest_minute_, latest_minute_)(rng);
    draw_side_ = std::bernoulli_distribution(0.5)(rng) ? model::OptionType::CALL
                                                       : model::OptionType::PUT;
    drawn_for_ = day;
}

auto RandomBuyStrategy::entry_signal(int minute) -> std::optional<model::OptionType> {
    if (drawn_for_ != day()) {
        draw(day());
    }
    if (minute < draw_minute_) {
        return std::nullopt;
    }
    return draw_side_;
}

auto RandomBuyStrategy::exit_due(int minute) const -> bool {
    return minute >= entered_minute() + hold_minutes_ || ZeroDteBuyer::exit_due(minute);
}

REGISTER_STRATEGY(LateDayMomentumStrategy);
REGISTER_STRATEGY(OpeningRangeBreakoutStrategy);
REGISTER_STRATEGY(RandomBuyStrategy);

} // namespace strategy
