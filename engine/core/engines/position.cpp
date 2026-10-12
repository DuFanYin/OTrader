/** PositionEngine: each strategy's holding from its fills, valued at the portfolio's prices. */

#include "core/engines/position.hpp"
#include "model/portfolio_structure.hpp"
#include "model/symbol.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>

namespace core {

namespace {
/** HALF_UP rounding by decimal digits (factor-based); avoids 0.01 float representation issues. */
inline auto round_digits(double value, int digits) -> double {
    if (digits < 0) {
        return value;
    }
    const double factor = std::pow(10.0, digits);
    const double scaled = value * factor;
    const double rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
    return rounded / factor;
}

// ── Settlement at expiry

constexpr const char* kSettlement = "Settlement";
/** OCC exercises an option in the money by this much or more at expiry. */
constexpr double kExerciseThreshold = 0.01;
/** Quantities are whole contracts and shares: anything closer is the same. */
constexpr double kQuantityTolerance = 1e-6;

using Settlement = PositionEngine::Settlement;

auto closing(int quantity) -> model::Direction {
    return quantity > 0 ? model::Direction::SHORT : model::Direction::LONG;
}

auto intrinsic(const model::OptionId& id, double price) -> double {
    const double value =
        id.type == model::OptionType::CALL ? price - id.strike : id.strike - price;
    return std::round(std::max(value, 0.0) * 100.0) / 100.0;
}

auto settlement_order(const std::string& orderid, const std::string& symbol,
                      model::Direction direction, double volume, double price,
                      model::DateTime at) -> model::OrderData {
    model::OrderData order;
    order.symbol = symbol;
    order.orderid = orderid;
    order.type = model::OrderType::MARKET;
    order.direction = direction;
    order.price = price;
    order.volume = volume;
    order.traded = volume;
    order.status = model::Status::ALLTRADED;
    order.datetime = at;
    order.reference = kSettlement;
    return order;
}

auto settlement_fill(const model::OrderData& order, const std::string& tradeid,
                     const std::string& symbol, model::Direction direction, double volume,
                     double price) -> model::TradeData {
    model::TradeData trade;
    trade.symbol = symbol;
    trade.orderid = order.orderid;
    trade.tradeid = tradeid;
    trade.direction = direction;
    trade.price = price;
    trade.volume = volume;
    trade.datetime = order.datetime;
    return trade;
}

/** The shares an option in the money delivers at its strike; nullopt when it delivers none. */
auto delivery(const std::string& strategy_name, const std::string& symbol,
              const model::OptionId& id, int quantity, double multiplier, double price,
              const std::string& underlying, model::DateTime at) -> std::optional<Settlement> {
    if (intrinsic(id, price) < kExerciseThreshold) {
        return std::nullopt;
    }
    const bool call = id.type == model::OptionType::CALL;
    const model::Direction direction =
        (call == (quantity > 0)) ? model::Direction::LONG : model::Direction::SHORT;
    const double shares = std::abs(quantity) * multiplier;
    Settlement c;
    c.order = settlement_order(std::format("settle-{}-{}-delivery", strategy_name, symbol),
                               underlying, direction, shares, id.strike, at);
    c.fills.push_back(
        settlement_fill(c.order, c.order.orderid, underlying, direction, shares, id.strike));
    return c;
}
} // namespace

auto PositionCheck::is_mismatch() const -> bool {
    return traded_here && std::abs(engine - broker) >= kQuantityTolerance;
}

void PositionEngine::process_trade_event(const std::string& strategy_name,
                                         const model::TradeData& trade,
                                         const model::OrderData* order) {
    write_log("process_trade strategy=" + strategy_name + " orderid=" + trade.orderid +
                  " tradeid=" + trade.tradeid + " symbol=" + trade.symbol +
                  " volume=" + std::to_string(trade.volume),
              utilities::DEBUG);

    get_create_strategy_holding(strategy_name);
    model::StrategyHolding& holding = strategy_holdings_[strategy_name];

    if (order != nullptr && order->is_combo) {
        model::OptionPositionData& combo = get_or_create_combo_position(holding, *order);
        const auto leg =
            std::ranges::find(combo.legs, trade.symbol, &model::OptionPositionData::symbol);
        if (leg == combo.legs.end()) {
            write_log(std::format("Fill {} of {} is not a leg of combo order {}: booked as a "
                                  "single option",
                                  trade.tradeid, trade.symbol, trade.orderid),
                      utilities::WARNING);
            apply_single_leg_option_trade(holding, trade);
            return;
        }
        apply_position_change(&*leg, trade);
        combo.quantity = combo.combo_quantity();
        write_log(std::format("combo {}: leg {} {}, combo {}", combo.symbol, leg->symbol,
                              leg->quantity, combo.quantity),
                  utilities::DEBUG);
        return;
    }

    if (!model::parse_option_symbol(trade.symbol)) { // not an option: the underlying
        apply_underlying_trade(holding, trade);
        return;
    }

    apply_single_leg_option_trade(holding, trade);
}

void PositionEngine::get_create_strategy_holding(const std::string& strategy_name) {
    if (!strategy_holdings_.contains(strategy_name)) {
        strategy_holdings_[strategy_name] = model::StrategyHolding();
    }
}

void PositionEngine::remove_strategy_holding(const std::string& strategy_name) {
    strategy_holdings_.erase(strategy_name);
    attribution_starts_.erase(strategy_name);
}

auto PositionEngine::get_holding(const std::string& strategy_name) -> model::StrategyHolding& {
    return strategy_holdings_.at(strategy_name);
}

auto PositionEngine::find_holding(const std::string& strategy_name) const
    -> const model::StrategyHolding* {
    const auto it = strategy_holdings_.find(strategy_name);
    return it != strategy_holdings_.end() ? &it->second : nullptr;
}

auto PositionEngine::check_positions(const std::map<std::string, double>& broker,
                                     const model::PortfolioStructure& portfolios) const
    -> std::vector<PositionCheck> {
    std::map<std::string, double> held;
    for (const auto& [name, holding] : strategy_holdings_) {
        const model::UnderlyingPositionData& shares = holding.underlyingPosition;
        if (shares.quantity != 0 && !shares.symbol.empty()) {
            held[shares.symbol] += shares.quantity;
        }
        holding.for_each_open_option(
            [&held](const model::BasePosition& p) { held[p.symbol] += p.quantity; });
    }
    std::erase_if(held, [](const auto& kv) { return std::abs(kv.second) < kQuantityTolerance; });

    std::vector<PositionCheck> out;
    for (const auto& [symbol, quantity] : held) {
        const auto it = broker.find(symbol);
        out.push_back({.symbol = symbol,
                       .engine = quantity,
                       .broker = it != broker.end() ? it->second : 0.0,
                       .traded_here = true});
    }
    for (const auto& [symbol, quantity] : broker) {
        if (!held.contains(symbol)) {
            out.push_back(
                {.symbol = symbol, .broker = quantity, .traded_here = portfolios.trades_contract(symbol)});
        }
    }
    std::ranges::sort(out, {}, &PositionCheck::symbol);
    return out;
}

auto PositionEngine::held_options() const -> std::vector<std::string> {
    std::set<std::string> held;
    for (const auto& [name, holding] : strategy_holdings_) {
        holding.for_each_open_option(
            [&held](const model::BasePosition& p) { held.insert(p.symbol); });
    }
    return {held.begin(), held.end()};
}

void PositionEngine::record_broker_check(std::span<const PositionCheck> checks) {
    std::string problem;
    for (const PositionCheck& c : checks) {
        if (c.is_mismatch()) {
            problem += std::format("{}{}: engine {}, broker {}", problem.empty() ? "" : "; ",
                                   c.symbol, c.engine, c.broker);
        }
    }
    const BrokerCheck check = problem.empty() ? BrokerCheck::Agree : BrokerCheck::Disagree;
    if (check == broker_check_ && problem == broker_check_problem_) {
        return; // as last time (the account page checks on each refresh)
    }
    broker_check_ = check;
    broker_check_problem_ = problem;
    for (const PositionCheck& c : checks) {
        if (!c.traded_here && c.broker != 0.0) {
            write_log(std::format("Position in the account not traded here: {} {}", c.broker,
                                  c.symbol),
                      utilities::INFO);
        }
    }
    if (problem.empty()) {
        write_log("The account's positions agree with the holdings", utilities::INFO);
    } else {
        write_log("The account's positions disagree with the holdings: " + problem,
                  utilities::ERROR);
    }
}

void PositionEngine::record_broker_check_failed(const std::string& reason) {
    broker_check_ = BrokerCheck::Failed;
    broker_check_problem_ = reason;
    write_log("The account's positions were not checked against the holdings: " + reason,
              utilities::ERROR);
}

auto PositionEngine::broker_check() const -> std::pair<std::string, std::string> {
    switch (broker_check_) {
    case BrokerCheck::NotYet:
        return {"not yet", ""};
    case BrokerCheck::Agree:
        return {"agree", ""};
    case BrokerCheck::Disagree:
        return {"disagree", broker_check_problem_};
    case BrokerCheck::Failed:
        return {"failed", broker_check_problem_};
    }
    return {"not yet", ""};
}

auto PositionEngine::start_refusal(const std::string& strategy_name, model::DateTime now,
                                   bool broker_agreement_required) const
    -> std::optional<std::string> {
    if (const std::vector<std::string> expired = expired_options(strategy_name, now);
        !expired.empty()) {
        std::string symbols;
        for (const std::string& s : expired) {
            symbols += (symbols.empty() ? "" : ", ") + s;
        }
        return std::format(
            "{} holds expired options not settled ({}): settle them with SettleExpired",
            strategy_name, symbols);
    }
    if (!broker_agreement_required) {
        return std::nullopt;
    }
    switch (broker_check_) {
    case BrokerCheck::NotYet:
        return "Connect first: the account's positions are checked against the holdings on "
               "connect";
    case BrokerCheck::Failed:
        return std::format("The account's positions were not checked against the holdings ({}):"
                           " ReconcilePositions once the broker answers",
                           broker_check_problem_);
    case BrokerCheck::Disagree:
        return std::format("The account's positions disagree with the holdings ({}): set them"
                           " right, then ReconcilePositions",
                           broker_check_problem_);
    case BrokerCheck::Agree:
        break;
    }
    return std::nullopt;
}

auto PositionEngine::expired_options(const std::string& strategy_name,
                                     model::DateTime now) const -> std::vector<std::string> {
    std::vector<std::string> out;
    if (const model::StrategyHolding* holding = find_holding(strategy_name)) {
        holding->for_each_open_option([&](const model::BasePosition& p) {
            const auto id = model::parse_option_symbol(p.symbol);
            if (id && model::option_expiry_time(id->expiry) <= now) {
                out.push_back(p.symbol);
            }
        });
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

auto PositionEngine::settle_expiry(const std::string& strategy_name,
                                   std::chrono::year_month_day expiry, double price,
                                   const model::PortfolioData& portfolio) const
    -> std::vector<Settlement> {
    const model::StrategyHolding* holding = find_holding(strategy_name);
    if (holding == nullptr) {
        return {};
    }
    const bool physical = portfolio.physical;
    const std::string& underlying = portfolio.underlying_symbol;
    const model::DateTime at = model::option_expiry_time(expiry);
    // Booked once the holding is walked: booking may change it.
    std::vector<Settlement> closes;
    std::vector<Settlement> deliveries;

    for (const auto& [key, position] : holding->optionPositions) {
        if (position.legs.empty()) { // a single option
            const std::optional<model::OptionId> id = model::parse_option_symbol(key);
            if (position.quantity == 0 || !id || id->expiry != expiry) {
                continue;
            }
            const double close = physical ? 0.0 : intrinsic(*id, price);
            const double volume = std::abs(position.quantity);
            Settlement c;
            c.order = settlement_order(std::format("settle-{}-{}", strategy_name, key), key,
                                       closing(position.quantity), volume, close, at);
            c.fills.push_back(settlement_fill(c.order, c.order.orderid, key,
                                              closing(position.quantity), volume, close));
            closes.push_back(std::move(c));
            if (physical) {
                if (auto d = delivery(strategy_name, key, *id, position.quantity,
                                      position.multiplier, price, underlying, at)) {
                    deliveries.push_back(std::move(*d));
                }
            }
            continue;
        }

        // A combo: the legs expiring now are closed; its quantity follows the remaining legs.
        Settlement c;
        double value = 0.0; // what closing the expiring legs is worth, per the legs' quantities
        for (const auto& leg : position.legs) {
            const std::optional<model::OptionId> id =
                model::parse_option_symbol(leg.symbol);
            if (leg.quantity == 0) {
                continue;
            }
            if (!id || id->expiry != expiry) {
                continue;
            }
            const double close = physical ? 0.0 : intrinsic(*id, price);
            value += close * leg.quantity;
            c.fills.push_back(settlement_fill(
                c.order,
                std::format("settle-{}-{}-{}-{:%Y%m%d}", strategy_name, key, leg.symbol, expiry),
                leg.symbol, closing(leg.quantity), std::abs(leg.quantity), close));
            model::Leg settled;
            settled.symbol = leg.symbol;
            settled.direction = closing(leg.quantity);
            settled.ratio = position.quantity != 0 ? std::abs(leg.quantity / position.quantity) : 1;
            c.order.legs.push_back(settled);
            if (physical) {
                if (auto d = delivery(strategy_name, leg.symbol, *id, leg.quantity, leg.multiplier,
                                      price, underlying, at)) {
                    deliveries.push_back(std::move(*d));
                }
            }
        }
        if (c.fills.empty()) {
            continue;
        }
        const double volume = std::abs(position.quantity);
        auto legs = std::move(c.order.legs);
        c.order =
            settlement_order(std::format("settle-{}-{}-{:%Y%m%d}", strategy_name, key, expiry), key,
                             closing(position.quantity), volume,
                             position.quantity != 0 ? value / position.quantity : 0.0, at);
        c.order.is_combo = true;
        c.order.combo_type = position.combo_type.value_or(model::ComboType::CUSTOM);
        c.order.legs = std::move(legs);
        for (auto& fill : c.fills) {
            fill.orderid = c.order.orderid;
            fill.datetime = at;
        }
        closes.push_back(std::move(c));
    }

    closes.insert(closes.end(), std::make_move_iterator(deliveries.begin()),
                  std::make_move_iterator(deliveries.end()));
    return closes;
}

void PositionEngine::apply_underlying_trade(model::StrategyHolding& holding,
                                            const model::TradeData& trade) {
    model::UnderlyingPositionData& pos = holding.underlyingPosition;
    if (pos.symbol.empty()) {
        pos.symbol = trade.symbol;
    }
    apply_position_change(&pos, trade);
}

void PositionEngine::apply_single_leg_option_trade(model::StrategyHolding& holding,
                                                   const model::TradeData& trade) {
    auto it = holding.optionPositions.find(trade.symbol);
    if (it == holding.optionPositions.end()) {
        holding.optionPositions[trade.symbol] = model::OptionPositionData(trade.symbol);
        it = holding.optionPositions.find(trade.symbol);
    }
    apply_position_change(&it->second, trade);
}

namespace {
/** A combo order's legs, sorted by symbol, each with its signed ratio (bought +, sold -), negated
 * so that the first ratio is positive: equal for every order over the same legs. */
auto combo_legs(const model::OrderData& order) -> std::vector<std::pair<std::string, int>> {
    std::vector<std::pair<std::string, int>> legs;
    for (const model::Leg& leg : order.legs) {
        const int side = leg.direction == model::Direction::SHORT ? -1 : 1;
        legs.emplace_back(leg.symbol, side * std::max(1, std::abs(leg.ratio)));
    }
    std::ranges::sort(legs);
    if (!legs.empty() && legs.front().second < 0) {
        for (auto& [_, ratio] : legs) {
            ratio = -ratio;
        }
    }
    return legs;
}

/** A combo position's key: the sorted leg symbols joined by '|'. */
auto combo_key(std::span<const std::pair<std::string, int>> legs) -> std::string {
    std::string key;
    for (const auto& [symbol, _] : legs) {
        key += (key.empty() ? "" : "|") + symbol;
    }
    return key;
}

auto contains_all_legs(const model::OptionPositionData& combo,
                       std::span<const std::pair<std::string, int>> legs) -> bool {
    return std::ranges::all_of(legs, [&](const auto& leg) {
        return std::ranges::contains(combo.legs, leg.first, &model::OptionPositionData::symbol);
    });
}
} // namespace

auto PositionEngine::get_or_create_combo_position(model::StrategyHolding& holding,
                                                  const model::OrderData& order)
    -> model::OptionPositionData& {
    const auto legs = combo_legs(order);
    const std::string key = combo_key(legs);
    model::OptionPositionData* combo = nullptr;
    if (const auto it = holding.optionPositions.find(key); it != holding.optionPositions.end()) {
        combo = &it->second;
    } else {
        const std::string* first = nullptr;
        for (auto& [k, held] : holding.optionPositions) {
            if (!held.legs.empty() && (first == nullptr || k < *first) &&
                contains_all_legs(held, legs)) {
                combo = &held;
                first = &k;
            }
        }
    }
    if (combo == nullptr) {
        combo = &holding.optionPositions[key];
        combo->symbol = key;
        for (const auto& [symbol, ratio] : legs) {
            combo->legs.emplace_back(symbol).ratio = ratio;
        }
    }
    // A specific combo type replaces CUSTOM (a closing order is CUSTOM).
    if (!combo->combo_type || (*combo->combo_type == model::ComboType::CUSTOM &&
                               order.combo_type != model::ComboType::CUSTOM)) {
        combo->combo_type = order.combo_type;
    }
    return *combo;
}

void PositionEngine::apply_position_change(model::BasePosition* pos,
                                           const model::TradeData& trade) {
    int qty = static_cast<int>(std::abs(trade.volume));
    int signed_qty = (trade.direction == model::Direction::LONG) ? qty : -qty;
    int prev_qty = pos->quantity;
    double multiplier = pos->multiplier;

    if (prev_qty == 0 || (prev_qty > 0 && signed_qty > 0) || (prev_qty < 0 && signed_qty < 0)) {
        int total_qty = std::abs(prev_qty) + qty;
        if (prev_qty == 0) {
            pos->avg_cost = round_digits(trade.price, 2);
        } else {
            pos->avg_cost = round_digits(
                (pos->avg_cost * std::abs(prev_qty) + trade.price * qty) / total_qty, 2);
        }
        pos->quantity += signed_qty;
        pos->cost_value = round_digits(pos->avg_cost * pos->quantity * multiplier, 2);
        return;
    }

    int close_qty = std::min(std::abs(prev_qty), qty);
    double pnl = (prev_qty > 0) ? (trade.price - pos->avg_cost) * close_qty
                                : (pos->avg_cost - trade.price) * close_qty;
    pos->realized_pnl += round_digits(pnl * multiplier, 2);

    int new_qty = std::abs(prev_qty) - close_qty;
    if (new_qty == 0) {
        pos->quantity = 0;
        pos->avg_cost = 0.0;
        pos->cost_value = 0.0;
    } else {
        pos->quantity = (prev_qty > 0 ? 1 : -1) * new_qty;
        pos->cost_value = round_digits(pos->avg_cost * pos->quantity * multiplier, 2);
    }

    int extra = qty - close_qty;
    if (extra > 0) {
        pos->avg_cost = round_digits(trade.price, 2);
        pos->quantity = (signed_qty > 0 ? 1 : -1) * extra;
        pos->cost_value = round_digits(pos->avg_cost * pos->quantity * multiplier, 2);
    }
}

namespace {
/** What a position adds to its holding's summary, valued at its mid and Greeks as set. */
auto position_totals(const model::BasePosition& pos) -> model::PortfolioSummary {
    return {.total_cost = round_digits(pos.cost_value, 2),
            .current_value = round_digits(pos.current_value(), 2),
            .realized_pnl = round_digits(pos.realized_pnl, 2),
            .delta = round_digits(pos.quantity * pos.delta, 4),
            .gamma = round_digits(pos.quantity * pos.gamma, 4),
            .theta = round_digits(pos.quantity * pos.theta, 4),
            .vega = round_digits(pos.quantity * pos.vega, 4)};
}
} // namespace

auto PositionEngine::accumulate_position(model::BasePosition* pos,
                                         const model::OptionData* option_snapshot)
    -> model::PortfolioSummary {
    double delta = 0;
    double gamma = 0;
    double theta = 0;
    double vega = 0;
    double mid_price = 0;
    if (option_snapshot != nullptr) {
        delta = option_snapshot->delta;
        gamma = option_snapshot->gamma;
        theta = option_snapshot->theta;
        vega = option_snapshot->vega;
        mid_price = option_snapshot->mid_price;
    }
    pos->delta = round_digits(delta, 4);
    pos->gamma = round_digits(gamma, 4);
    pos->theta = round_digits(theta, 4);
    pos->vega = round_digits(vega, 4);
    pos->mid_price = round_digits(mid_price, 2);
    return position_totals(*pos);
}

auto PositionEngine::accumulate_position(model::BasePosition* pos,
                                         const model::UnderlyingData* underlying_snapshot)
    -> model::PortfolioSummary {
    double delta = 1.0;
    double mid_price = 0;
    if (underlying_snapshot != nullptr) {
        delta = underlying_snapshot->theo_delta;
        mid_price = underlying_snapshot->mid_price;
    }
    pos->delta = round_digits(delta, 4);
    pos->mid_price = round_digits(mid_price, 2);
    return position_totals(*pos);
}

auto PositionEngine::accumulate_option_position(model::OptionPositionData& opt,
                                                const model::PortfolioData& portfolio)
    -> model::PortfolioSummary {
    double current_value = 0.0;
    double total_cost = 0.0;
    double realized = 0.0;
    model::PortfolioSummary greeks; // its Greeks times its quantity: what it adds to the holding

    std::vector<model::BasePosition*> components;
    if (opt.legs.empty()) {
        components.push_back(&opt);
    } else {
        for (auto& leg : opt.legs) {
            components.push_back(&leg);
        }
    }

    for (auto* leg : components) {
        const model::OptionData* inst = nullptr;
        auto it = portfolio.options.find(leg->symbol);
        if (it != portfolio.options.end()) {
            inst = &it->second;
        }
        const model::PortfolioSummary acc = accumulate_position(leg, inst);
        current_value += acc.current_value;
        total_cost += acc.total_cost;
        realized += acc.realized_pnl;
        greeks.delta += acc.delta;
        greeks.gamma += acc.gamma;
        greeks.theta += acc.theta;
        greeks.vega += acc.vega;
    }
    if (!opt.legs.empty()) {
        // A combo shows its legs' total; a single option keeps its own, per contract, as
        // accumulate_position set them.
        opt.delta = greeks.delta;
        opt.gamma = greeks.gamma;
        opt.theta = greeks.theta;
        opt.vega = greeks.vega;
    }

    opt.cost_value = round_digits(total_cost, 2);
    opt.realized_pnl = round_digits(realized, 2);

    if (opt.quantity != 0) {
        opt.mid_price = round_digits(current_value / (std::abs(opt.quantity) * opt.multiplier), 2);
        if (opt.cost_value != 0.0) {
            const double units = std::abs(opt.quantity) * opt.multiplier;
            opt.avg_cost = round_digits(std::abs(opt.cost_value) / units, 2);
        }
    }

    return {.total_cost = round_digits(opt.cost_value, 2),
            .current_value = round_digits(current_value, 2),
            .realized_pnl = round_digits(opt.realized_pnl, 2),
            .delta = round_digits(greeks.delta, 4),
            .gamma = round_digits(greeks.gamma, 4),
            .theta = round_digits(greeks.theta, 4),
            .vega = round_digits(greeks.vega, 4)};
}

void PositionEngine::add_totals(model::PortfolioSummary& totals,
                                const model::PortfolioSummary& metrics) {
    totals.current_value += metrics.current_value;
    totals.total_cost += metrics.total_cost;
    totals.realized_pnl += metrics.realized_pnl;
    totals.delta += metrics.delta;
    totals.gamma += metrics.gamma;
    totals.theta += metrics.theta;
    totals.vega += metrics.vega;
}

auto PositionEngine::shares_instrument(const model::StrategyHolding& holding,
                                       const model::PortfolioData& portfolio)
    -> const model::UnderlyingData* {
    // The hedge instrument when that is what was traded (SPY for SPXW), valued at its price and
    // counted in the underlying's units by its theo_delta.
    return portfolio.hedge_instrument &&
                   portfolio.hedge_instrument->symbol == holding.underlyingPosition.symbol
               ? portfolio.hedge_instrument.get()
               : portfolio.underlying.get();
}

void PositionEngine::attribute_pnl(const std::string& strategy_name,
                                   model::StrategyHolding& holding,
                                   const model::PortfolioData& portfolio) {
    // Prices and Greeks are the portfolio's own at both ends, not the position's (rounded): a
    // rounding then cancels out from one update to the next instead of adding up.
    model::PortfolioSummary& s = holding.summary;
    const model::DateTime now = portfolio.datetime();
    const double underlying = portfolio.underlying ? portfolio.underlying->mid_price : 0.0;
    const model::UnderlyingData* instrument = shares_instrument(holding, portfolio);
    const double shares_price = instrument != nullptr ? instrument->mid_price : 0.0;

    AttributionStart& start = attribution_starts_[strategy_name];
    double moved = 0.0; // what the holding at the start gained by now, at mids
    if (start.time != model::DateTime{}) {
        const double move = start.underlying_price > 0.0 && underlying > 0.0
                                ? underlying - start.underlying_price
                                : 0.0;
        const double days =
            std::chrono::duration<double>(now - start.time).count() / (24.0 * 3600.0);
        for (std::size_t i = 0; i < start.options.size(); ++i) {
            const model::BasePosition& was = start.options[i];
            const auto it = portfolio.options.find(was.symbol);
            if (it == portfolio.options.end() || it->second.mid_price <= 0.0 ||
                was.mid_price <= 0.0) {
                continue; // no mark at one end: its change comes in with pnl_fills
            }
            const double q = was.quantity;
            const double gained = q * (it->second.mid_price - was.mid_price) * was.multiplier;
            const double iv_now = it->second.mid_iv;
            const double vol_points = start.option_ivs[i] > 0.0 && iv_now > 0.0
                                          ? (iv_now - start.option_ivs[i]) * 100.0
                                          : 0.0;
            const double delta = q * was.delta * move;
            const double gamma = 0.5 * q * was.gamma * move * move;
            const double theta = q * was.theta * days;
            const double vega = q * was.vega * vol_points;
            s.pnl_delta += delta;
            s.pnl_gamma += gamma;
            s.pnl_theta += theta;
            s.pnl_vega += vega;
            s.pnl_other += gained - delta - gamma - theta - vega;
            moved += gained;
        }
        if (start.shares.quantity != 0 && start.shares.mid_price > 0.0 && shares_price > 0.0) {
            const double gained = start.shares.quantity *
                                  (shares_price - start.shares.mid_price) *
                                  start.shares.multiplier;
            s.pnl_delta += gained;
            moved += gained;
        }
    }
    if (start.time != model::DateTime{}) {
        s.pnl_fills += s.pnl - start.pnl - moved;
    } // the first update is where it starts: what the holding made before (rebooked) is not split

    start.time = now;
    start.pnl = s.pnl;
    start.underlying_price = underlying;
    start.shares = holding.underlyingPosition;
    start.shares.mid_price = shares_price;
    start.options.clear();
    start.option_ivs.clear();
    holding.for_each_open_option([&](const model::BasePosition& position) {
        model::BasePosition was = position;
        double iv = 0.0;
        if (const auto it = portfolio.options.find(position.symbol); it != portfolio.options.end()) {
            was.mid_price = it->second.mid_price;
            was.delta = it->second.delta;
            was.gamma = it->second.gamma;
            was.theta = it->second.theta;
            was.vega = it->second.vega;
            iv = it->second.mid_iv;
        }
        start.options.push_back(was);
        start.option_ivs.push_back(iv);
    });
}

void PositionEngine::update_metrics(const std::string& strategy_name,
                                    const model::PortfolioData& portfolio) {
    const auto found = strategy_holdings_.find(strategy_name);
    if (found == strategy_holdings_.end()) {
        return;
    }
    model::StrategyHolding& holding = found->second;

    model::PortfolioSummary totals;
    for (auto& kv : holding.optionPositions) {
        add_totals(totals, accumulate_option_position(kv.second, portfolio));
    }

    if (holding.underlyingPosition.quantity != 0 || holding.underlyingPosition.realized_pnl != 0) {
        add_totals(totals, accumulate_position(&holding.underlyingPosition,
                                               shares_instrument(holding, portfolio)));
    }

    double unreal = totals.current_value - totals.total_cost;
    holding.summary.current_value = round_digits(totals.current_value, 2);
    holding.summary.total_cost = round_digits(totals.total_cost, 2);
    holding.summary.unrealized_pnl = round_digits(unreal, 2);
    holding.summary.realized_pnl = round_digits(totals.realized_pnl, 2);
    holding.summary.pnl = holding.summary.unrealized_pnl + holding.summary.realized_pnl;
    holding.summary.delta = round_digits(totals.delta, 4);
    holding.summary.gamma = round_digits(totals.gamma, 4);
    holding.summary.theta = round_digits(totals.theta, 4);
    holding.summary.vega = round_digits(totals.vega, 4);
    attribute_pnl(strategy_name, holding, portfolio);

    for (auto& kv : holding.optionPositions) {
        kv.second.clear_fields();
    }
    holding.underlyingPosition.clear_fields();
}

} // namespace core
