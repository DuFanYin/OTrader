#include "random_buy.hpp"
#include "strategy_registry.hpp"

#include <algorithm>
#include <cstdint>
#include <random>

namespace strategy {

void RandomBuyStrategy::draw(std::chrono::year_month_day day) {
    // Deterministic in seed and day, so backtest and live draw alike.
    const auto days = std::chrono::sys_days{day}.time_since_epoch().count();
    std::mt19937_64 rng(static_cast<std::uint64_t>(setting_.seed.value()) * 1'000'003ULL +
                        static_cast<std::uint64_t>(days));
    const int earliest = setting_.earliest_minute;
    const int latest = std::max(setting_.latest_minute.value(), earliest);
    draw_minute_ = std::uniform_int_distribution<int>(earliest, latest)(rng);
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
    return minute >= entered_minute() + setting_.hold_minutes || ZeroDteBuyer::exit_due(minute);
}

REGISTER_STRATEGY(RandomBuyStrategy);

} // namespace strategy
