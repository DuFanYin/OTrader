/** Strategy: the base every strategy derives from. */

#include "strategy/strategy.hpp"
#include "quant/kernel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace strategy {

auto Strategy::base_parameters() -> std::vector<model::Parameter> {
    return model::parameters_of<BaseSetting>();
}

void Strategy::on_order(const model::Order& order) {
    const std::string side =
        order.is_combo() ? "" : model::to_string(order.legs.front().side) + " ";
    write_log(std::format("Order {}: {}{} {} @ {} [{}]", order.order_id, side, order.quantity,
                          order.symbol, order.price, model::to_string(order.status)));
}

void Strategy::on_fill(const model::Fill& fill) {
    write_log(std::format("Fill {}: {} {} {} @ {}", fill.fill_id,
                          model::to_string(fill.side), fill.quantity, fill.symbol,
                          fill.price));
}

void Strategy::subscribe_chains(std::span<const std::string> chain_symbols) {
    subscribed_chains_ = {chain_symbols.begin(), chain_symbols.end()};
    context_.subscribe_chains(chain_symbols);
}

auto Strategy::find_chain(const std::string& chain_symbol) const
    -> std::optional<model::Chain> {
    if (!subscribed_chains_.contains(chain_symbol)) {
        return std::nullopt;
    }
    return portfolio()->chain(chain_symbol);
}

auto Strategy::select_chain(int min_dte, int max_dte) -> std::string {
    const std::vector<std::string> chains = portfolio()->chains_by_expiry(min_dte, max_dte);
    if (chains.empty()) {
        return {};
    }
    subscribe_chains(std::span(&chains.front(), 1));
    return chains.front();
}

auto Strategy::option_at(const model::Chain& chain, model::OptionType type,
                         int steps) const -> std::optional<model::Option> {
    const double atm_strike = chain.atm_strike();
    if (atm_strike <= 0) {
        return std::nullopt;
    }
    const std::span<const double> strikes = chain.strikes();
    const auto atm = std::ranges::find(strikes, atm_strike);
    const auto at = (atm - strikes.begin()) + steps;
    if (atm == strikes.end() || at < 0 || std::cmp_greater_equal(at, strikes.size())) {
        return std::nullopt;
    }
    return chain.option(type, strikes[static_cast<std::size_t>(at)]);
}

auto Strategy::holds_options() const -> bool {
    bool held = false;
    context_.holding().for_each_open_option([&held](const model::Position&) { held = true; });
    return held;
}

auto Strategy::daily(std::string_view series) const -> std::optional<double> {
    const std::vector<double> last = daily_history(series, 1);
    return last.empty() ? std::nullopt : std::optional(last.back());
}

auto Strategy::daily_history(std::string_view series, std::size_t n) const
    -> std::vector<double> {
    return context_.daily().before(series, day(), n);
}

auto Strategy::trading_days_until(std::string_view series) const -> int {
    const std::chrono::year_month_day today = day();
    const std::optional<std::chrono::year_month_day> next = context_.daily().next(series, today);
    if (!next) {
        return -1;
    }
    int days = 0;
    for (std::chrono::sys_days d{today}; d < std::chrono::sys_days{*next}; d += std::chrono::days{1}) {
        days += model::is_trading_day(std::chrono::year_month_day{d}) ? 1 : 0;
    }
    return days;
}

auto Strategy::realized_vol(std::string_view closes, int n) const -> std::optional<double> {
    const std::vector<double> c = daily_history(closes, static_cast<std::size_t>(std::max(n, 1)) + 1);
    const double vol = quant::realized_vol(c);
    return std::isfinite(vol) ? std::optional(vol) : std::nullopt;
}

auto Strategy::rank_in(std::string_view series, double value, int n) const
    -> std::optional<double> {
    const std::vector<double> h = daily_history(series, static_cast<std::size_t>(std::max(n, 1)));
    const double rank = quant::percentile_rank(h, value);
    return std::isfinite(rank) ? std::optional(rank) : std::nullopt;
}

auto Strategy::traded_today() const -> bool { return last_fill_minute() >= 0; }

namespace {

/** Minutes since the open of `at` when it is on `day` (New York), else -1. */
auto minute_on(model::DateTime at, std::chrono::year_month_day day) -> int {
    if (at == model::DateTime{} || model::new_york_date(at) != day) {
        return -1;
    }
    return std::max(model::minutes_since_open(at), 0);
}

} // namespace

auto Strategy::last_fill_minute() const -> int {
    return minute_on(context_.holding().last_fill, day());
}

auto Strategy::last_order_minute() const -> int {
    return minute_on(context_.holding().last_order, day());
}

auto Strategy::leg(const std::string& symbol, model::Side side,
                   int ratio) const -> model::Leg {
    const model::Contract* contract = portfolio()->contract(symbol);
    model::Leg leg = contract != nullptr ? model::contract_leg(*contract, side)
                                         : model::Leg{.side = side, .symbol = symbol};
    leg.ratio = ratio;
    return leg;
}

auto Strategy::combo_legs(
    model::ComboType combo_type,
    const std::unordered_map<std::string, std::string>& roles,
    model::Side side) const -> std::vector<model::Leg> {
    try {
        return model::build_combo(roles, combo_type, side,
                                  [this](const std::string& symbol) {
                                      return portfolio()->contract(symbol);
                                  })
            .first;
    } catch (const std::exception& e) {
        write_log(std::format("No {}: {}", model::to_string(combo_type), e.what()),
                  utilities::ERROR);
        return {};
    }
}

auto Strategy::send_order(std::span<const model::Leg> legs, double price, double quantity,
                          model::OrderType order_type, model::ComboType combo_type)
    -> std::string {
    if (legs.empty()) {
        return {};
    }
    for (const model::Leg& leg : legs) {
        if (portfolio()->contract(leg.symbol) == nullptr) {
            write_log("No contract " + leg.symbol + ": order not sent", utilities::WARNING);
            return {};
        }
    }
    return context_.send_order(model::order_request(legs, price, quantity, order_type, combo_type));
}

void Strategy::register_hedging(int timer_trigger, int delta_target,
                                int delta_range) {
    context_.register_hedging(timer_trigger, delta_target, delta_range);
}

void Strategy::unregister_hedging() { context_.unregister_hedging(); }

void Strategy::close_all_strategy_positions() {
    const model::StrategyHolding& holding = context_.holding();
    for (const auto& [_, options] :
         holding.open_options_by_combo([](const model::OptionPosition&) { return true; })) {
        close_options(options);
    }
    // Shares: of whatever they are in (the hedge instrument, SPY for SPXW, or the underlying).
    const model::SharePosition& shares = holding.shares;
    if (shares.quantity != 0 && !shares.symbol.empty()) {
        const model::Side dir =
            shares.quantity > 0 ? model::Side::SELL : model::Side::BUY;
        const std::array legs{leg(shares.symbol, dir)};
        (void)send_order(legs, 0.0, std::abs(static_cast<double>(shares.quantity)));
    }
}

void Strategy::close_options(std::span<const model::OptionPosition> options) {
    const auto order = model::closing_order(
        options, [this](const std::string& symbol) { return portfolio()->contract(symbol); });
    if (!order) {
        write_log(order.error(), utilities::WARNING);
        return;
    }
    (void)context_.send_order(*order);
}

auto Strategy::has_active_order() const -> bool { return context_.has_active_order(); }

void Strategy::set_error(const std::string& msg) { context_.set_error(msg); }

void Strategy::write_log(const std::string& msg, utilities::LogLevel level) const {
    context_.write_log(msg, level);
}

} // namespace strategy
