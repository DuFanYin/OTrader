#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "model/order_builder.hpp"
#include "model/portfolios.hpp"
#include "model/symbol.hpp"
#include "utilities/text.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
#include <set>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace core {

namespace {

/** HALF_UP rounding by decimal digits (factor-based); avoids 0.01 float representation issues. */
inline auto round_digits(double value, int digits) -> double {
    if (digits < 0) {
        return value;
    }
    // Exact powers of ten, the values std::pow gives for them, without its call.
    static constexpr std::array<double, 9> kPowersOfTen{1e0, 1e1, 1e2, 1e3, 1e4,
                                                        1e5, 1e6, 1e7, 1e8};
    const double factor = std::cmp_less(digits, kPowersOfTen.size())
                              ? kPowersOfTen[static_cast<std::size_t>(digits)]
                              : std::pow(10.0, digits);
    const double scaled = value * factor;
    const double rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
    return rounded / factor;
}

/** Quantities are whole contracts and shares: anything closer is the same. */
constexpr double kQuantityTolerance = 1e-6;

void apply_position_change(model::Position& pos, const model::Fill& fill) {
    int qty = static_cast<int>(std::abs(fill.quantity));
    int signed_qty = (fill.side == model::Side::BUY) ? qty : -qty;
    int prev_qty = pos.quantity;
    double multiplier = pos.multiplier;

    if (prev_qty == 0 || (prev_qty > 0 && signed_qty > 0) || (prev_qty < 0 && signed_qty < 0)) {
        int total_qty = std::abs(prev_qty) + qty;
        // Not rounded: a backtest's fills can fall inside the spread; the P&L is to the cent.
        pos.avg_cost = prev_qty == 0
                           ? fill.price
                           : (pos.avg_cost * std::abs(prev_qty) + fill.price * qty) / total_qty;
        pos.quantity += signed_qty;
        pos.cost_value = round_digits(pos.avg_cost * pos.quantity * multiplier, 2);
        return;
    }

    int close_qty = std::min(std::abs(prev_qty), qty);
    double pnl = (prev_qty > 0) ? (fill.price - pos.avg_cost) * close_qty
                                : (pos.avg_cost - fill.price) * close_qty;
    pos.realized_pnl += round_digits(pnl * multiplier, 2);

    int new_qty = std::abs(prev_qty) - close_qty;
    int extra = qty - close_qty;
    if (extra > 0) { // closed, and the rest opens the other way
        pos.avg_cost = fill.price;
        pos.quantity = (signed_qty > 0 ? 1 : -1) * extra;
        pos.cost_value = round_digits(pos.avg_cost * pos.quantity * multiplier, 2);
    } else if (new_qty == 0) {
        pos.quantity = 0;
        pos.avg_cost = 0.0;
        pos.cost_value = 0.0;
    } else {
        pos.quantity = (prev_qty > 0 ? 1 : -1) * new_qty;
        pos.cost_value = round_digits(pos.avg_cost * pos.quantity * multiplier, 2);
    }
}

void apply_shares_fill(model::StrategyHolding& holding, const model::Fill& fill) {
    model::SharePosition& pos = holding.shares;
    if (pos.symbol.empty()) {
        pos.symbol = fill.symbol;
    }
    apply_position_change(pos, fill);
}

/** What a position adds to its holding's summary, valued at its mid and Greeks as set. */
auto position_totals(const model::Position& pos) -> model::HoldingSummary {
    return {.total_cost = round_digits(pos.cost_value, 2),
            .current_value = round_digits(pos.current_value(), 2),
            .realized_pnl = round_digits(pos.realized_pnl, 2),
            .delta = round_digits(pos.quantity * pos.delta, 4),
            .gamma = round_digits(pos.quantity * pos.gamma, 4),
            .theta = round_digits(pos.quantity * pos.theta, 4),
            .vega = round_digits(pos.quantity * pos.vega, 4)};
}

/** A position's contribution to its holding's summary; also sets its own Greeks and mid. */
auto mark_option(model::Position& pos, const std::optional<model::Option>& option)
    -> model::HoldingSummary {
    const model::Greeks g = option ? option->greeks() : model::Greeks{};
    pos.delta = round_digits(g.delta, 4);
    pos.gamma = round_digits(g.gamma, 4);
    pos.theta = round_digits(g.theta, 4);
    pos.vega = round_digits(g.vega, 4);
    pos.mid_price = option ? option->mid() : 0.0; // a half-cent mid is the value: not rounded away
    return position_totals(pos);
}

/** The same for shares. */
auto mark_shares(model::Position& pos, const std::optional<model::Underlying>& shares)
    -> model::HoldingSummary {
    double delta = 1.0;
    double mid_price = 0;
    if (shares) {
        delta = shares->theo_delta();
        mid_price = shares->mid();
    }
    pos.delta = round_digits(delta, 4);
    pos.mid_price = mid_price;
    return position_totals(pos);
}

void add_totals(model::HoldingSummary& totals, const model::HoldingSummary& metrics) {
    totals.current_value += metrics.current_value;
    totals.total_cost += metrics.total_cost;
    totals.realized_pnl += metrics.realized_pnl;
    totals.delta += metrics.delta;
    totals.gamma += metrics.gamma;
    totals.theta += metrics.theta;
    totals.vega += metrics.vega;
}

auto shares_instrument(const model::StrategyHolding& holding, const model::Portfolio& portfolio)
    -> std::optional<model::Underlying> {
    // The hedge instrument when that is what was traded (SPY for SPXW), valued at its price and
    // counted in the underlying's units by its theo_delta.
    if (const std::optional<model::Underlying> hedge = portfolio.hedge();
        hedge && !portfolio.hedge_symbol.empty() &&
        hedge->symbol() == holding.shares.symbol) {
        return hedge;
    }
    return portfolio.underlying();
}

// ---- Settlement at expiry

constexpr const char* kSettlement = "Settlement";
/** OCC exercises an option in the money by this much or more at expiry. */
constexpr double kExerciseThreshold = 0.01;

using Settlement = BookkeepingEngine::Settlement;

auto closing(int quantity) -> model::Side {
    return quantity > 0 ? model::Side::SELL : model::Side::BUY;
}

auto intrinsic(const model::OptionId& id, double price) -> double {
    const double value =
        id.type == model::OptionType::CALL ? price - id.strike : id.strike - price;
    return std::round(std::max(value, 0.0) * 100.0) / 100.0;
}

/** A single contract's close-out or delivery: one order filled at once by one fill. */
auto single_settlement(const std::string& order_id, const std::string& symbol,
                       model::Side side, double quantity, double price,
                       model::DateTime at) -> Settlement {
    auto [order, fill] =
        model::filled_order(order_id, symbol, side, quantity, price, at, kSettlement);
    Settlement c;
    c.order = std::move(order);
    c.fills.push_back(std::move(fill));
    return c;
}

/** The shares an option in the money delivers at its strike; nullopt when it delivers none. */
auto delivery(const std::string& strategy_name, const std::string& symbol,
              const model::OptionId& id, int quantity, double multiplier, double price,
              const std::string& underlying, model::DateTime at) -> std::optional<Settlement> {
    if (intrinsic(id, price) < kExerciseThreshold) {
        return std::nullopt;
    }
    const bool call = id.type == model::OptionType::CALL;
    const model::Side side =
        (call == (quantity > 0)) ? model::Side::BUY : model::Side::SELL;
    const double shares = std::abs(quantity) * multiplier;
    return single_settlement(std::format("settle-{}-{}-delivery", strategy_name, symbol),
                             underlying, side, shares, id.strike, at);
}

constexpr std::chrono::minutes kSettleWindow{15}; // a quote this near the close is its price

/** The underlying's last quote if within kSettleWindow of the expiry's close; nullopt otherwise
 * (e.g. the next morning's quote is not the closing price). */
auto closing_price(const model::Portfolio& portfolio, std::chrono::year_month_day expiry)
    -> std::optional<double> {
    const std::optional<model::Underlying> underlying = portfolio.underlying();
    const double price = underlying ? underlying->mid() : 0.0;
    const auto from_close = portfolio.datetime() - model::option_expiry_time(expiry);
    if (price <= 0.0 || std::chrono::abs(from_close) > kSettleWindow) {
        return std::nullopt;
    }
    return price;
}

} // namespace

auto BrokerCheckRow::is_mismatch() const -> bool {
    return traded_here && std::abs(engine - broker) >= kQuantityTolerance;
}

// ---- Orders and fills

void BookkeepingEngine::add_order(const std::string& strategy_name, model::Order order) {
    if (order.order_id.empty()) {
        return;
    }
    if (order.datetime != model::DateTime{}) {
        model::StrategyHolding& holding = ensure_holding(strategy_name);
        holding.last_order = std::max(holding.last_order, order.datetime);
    }
    const std::string order_id = order.order_id;
    Tracked& t = orders_[order_id];
    if (!t.strategy_name.empty()) { // known already: what it counted goes first
        Books& books = books_[t.strategy_name];
        add_working(books, t.order, -t.working);
        books.active.erase(order_id);
    }
    t = Tracked{.order = std::move(order), .strategy_name = strategy_name};
    track_active(order_id, t);
}

auto BookkeepingEngine::update_order(const model::Order& update, model::DateTime now)
    -> std::optional<model::Order> {
    const auto [it, added] = orders_.try_emplace(update.order_id);
    Tracked& t = it->second;
    if (added) {
        t.order = update; // not sent here (another client's): as reported
        return t.order;
    }
    // A strategy's order ended before its fills came (IB reports a combo filled, then its legs):
    // held back, so a strategy told of the end can trust its holding.
    if (t.order.is_active() && !update.is_active() && update.filled > 0.0 &&
        !t.strategy_name.empty() && !fills_cover(t, update.filled)) {
        if (!t.held_end) {
            t.held_since = now;
        }
        t.held_end = update; // a repeated end replaces the one held
        return std::nullopt;
    }
    if (!merge(t, update)) {
        return std::nullopt; // nothing new: a repeat, or an older update come late
    }
    track_active(update.order_id, t);
    return t.order;
}

auto BookkeepingEngine::merge(Tracked& t, const model::Order& update) -> bool {
    model::Order& known = t.order;
    const double filled = std::max(known.filled, update.filled);
    if (!known.is_active()) { // an end is final: only what was filled may still rise
        if (filled == known.filled) {
            return false;
        }
        known.filled = filled;
        return true;
    }
    if (update.status == known.status && filled == known.filled) {
        return false;
    }
    known.status = update.status;
    known.filled = filled;
    if (update.datetime != model::DateTime{}) {
        known.datetime = update.datetime;
    }
    if (!known.is_active()) {
        t.held_end.reset();
    }
    return true;
}

auto BookkeepingEngine::book_fill(const model::Fill& fill) -> const std::string* {
    static const std::string kNone;
    if (!fill_index_.try_emplace(fill.fill_id, fills_.size()).second) {
        return nullptr;
    }
    fills_.push_back(fill);
    const auto it = orders_.find(fill.order_id);
    if (it == orders_.end()) {
        return &kNone;
    }
    Tracked& t = it->second;
    t.booked[fill.symbol] += fill.quantity;
    if (!t.strategy_name.empty()) {
        book_to_holding(t.strategy_name, fill, &t.order);
    }
    return &t.strategy_name;
}

auto BookkeepingEngine::fills_cover(const Tracked& t, double filled) -> bool {
    constexpr double kTolerance = 1e-9;
    return std::ranges::all_of(t.order.legs, [&](const model::Leg& leg) {
        const auto it = t.booked.find(leg.symbol);
        const double booked = it != t.booked.end() ? it->second : 0.0;
        return booked + kTolerance >= filled * std::max(1, std::abs(leg.ratio));
    });
}

auto BookkeepingEngine::take_released(const model::Fill& fill) -> std::optional<model::Order> {
    const auto it = orders_.find(fill.order_id);
    if (it == orders_.end() || !it->second.held_end ||
        !fills_cover(it->second, it->second.held_end->filled)) {
        return std::nullopt;
    }
    Tracked& t = it->second;
    const model::Order end = *std::exchange(t.held_end, std::nullopt);
    if (!merge(t, end)) {
        return std::nullopt;
    }
    track_active(fill.order_id, t);
    return t.order;
}

auto BookkeepingEngine::take_overdue(model::DateTime now) -> std::vector<model::Order> {
    std::vector<std::string> due;
    for (const auto& [order_id, t] : orders_) {
        if (t.held_end && now - t.held_since >= kTerminalUpdateTimeout) {
            due.push_back(order_id);
        }
    }
    std::ranges::sort(due); // deterministic order
    std::vector<model::Order> out;
    for (const std::string& order_id : due) {
        Tracked& t = orders_.at(order_id);
        const model::Order end = *std::exchange(t.held_end, std::nullopt);
        if (merge(t, end)) {
            track_active(order_id, t);
            out.push_back(t.order);
        }
    }
    return out;
}

auto BookkeepingEngine::cancel_sent(const std::string& order_id) -> bool {
    const auto it = orders_.find(order_id);
    if (it == orders_.end() || !is_active(order_id) || it->second.cancel_sent) {
        return false;
    }
    it->second.cancel_sent = true;
    return true;
}

void BookkeepingEngine::track_active(const std::string& order_id, Tracked& t) {
    if (t.strategy_name.empty()) {
        return; // no strategy's: never active here
    }
    Books& books = books_[t.strategy_name];
    const bool active = t.order.is_active();
    const double remaining = active ? std::max(t.order.quantity - t.order.filled, 0.0) : 0.0;
    if (remaining != t.working) {
        add_working(books, t.order, remaining - t.working);
        t.working = remaining;
    }
    if (active) {
        books.active[order_id] = &t;
    } else {
        books.active.erase(order_id);
    }
}

void BookkeepingEngine::add_working(Books& books, const model::Order& order, double units) {
    for (const model::Leg& leg : order.legs) {
        if (leg.symbol.empty() || units == 0.0) {
            continue;
        }
        const double sign = leg.side == model::Side::BUY ? 1.0 : -1.0;
        double& working = books.working[leg.symbol];
        working += sign * std::abs(static_cast<double>(leg.ratio)) * units;
        if (std::abs(working) < 1e-9) {
            books.working.erase(leg.symbol);
        }
    }
}

auto BookkeepingEngine::find_order(const std::string& order_id) const -> const model::Order* {
    const auto it = orders_.find(order_id);
    return it != orders_.end() ? &it->second.order : nullptr;
}

auto BookkeepingEngine::find_fill(const std::string& fill_id) const -> const model::Fill* {
    const auto it = fill_index_.find(fill_id);
    return it != fill_index_.end() ? &fills_[it->second] : nullptr;
}

auto BookkeepingEngine::strategy_of(const std::string& order_id) const
    -> const std::string& {
    static const std::string kNone;
    const auto it = orders_.find(order_id);
    return it != orders_.end() ? it->second.strategy_name : kNone;
}

auto BookkeepingEngine::is_active(const std::string& order_id) const -> bool {
    const auto it = orders_.find(order_id);
    return it != orders_.end() && !it->second.strategy_name.empty() &&
           it->second.order.is_active();
}

auto BookkeepingEngine::has_active_order(const std::string& strategy_name) const -> bool {
    const auto it = books_.find(strategy_name);
    return it != books_.end() && !it->second.active.empty();
}

void BookkeepingEngine::forget_finished() {
    std::unordered_set<std::string> done;
    std::erase_if(orders_, [&](const auto& kv) {
        if (kv.second.order.is_active() || kv.second.held_end) {
            return false;
        }
        done.insert(kv.first);
        return true;
    });
    forget_fills(done);
}

void BookkeepingEngine::forget_fills(const std::unordered_set<std::string>& order_ids) {
    if (order_ids.empty()) {
        return;
    }
    std::erase_if(fills_, [&](const model::Fill& f) { return order_ids.contains(f.order_id); });
    fill_index_.clear();
    for (std::size_t i = 0; i < fills_.size(); ++i) {
        fill_index_.emplace(fills_[i].fill_id, i);
    }
}

// ---- Holdings

void BookkeepingEngine::book_to_holding(const std::string& strategy_name, const model::Fill& fill,
                                        const model::Order* order) {
    model::StrategyHolding& holding = ensure_holding(strategy_name);
    holding.last_fill = std::max(holding.last_fill, fill.datetime);
    if (!model::parse_option_symbol(fill.symbol)) { // not an option: the underlying
        apply_shares_fill(holding, fill);
        return;
    }
    auto it = holding.options.find(fill.symbol);
    if (it == holding.options.end()) {
        if (auto closed = holding.closed_options.extract(fill.symbol)) {
            holding.closed_realized_pnl -= round_digits(closed.mapped().realized_pnl, 2);
            it = holding.options.insert(std::move(closed)).position;
        } else {
            it = holding.options.try_emplace(fill.symbol, fill.symbol).first;
        }
    }
    model::OptionPosition& position = it->second;
    const bool was_open = position.quantity != 0;
    if (!was_open) {
        const bool in_combo = order != nullptr && order->legs.size() > 1 &&
                              std::ranges::contains(order->legs, fill.symbol, &model::Leg::symbol);
        position.combo = in_combo ? model::generate_combo_signature(order->legs) : fill.symbol;
        position.combo_type = in_combo ? order->combo_type : model::ComboType::SINGLE_LEG;
    }
    apply_position_change(position, fill);
    held_options_changed_ |= was_open != (position.quantity != 0);
    if (position.quantity == 0) {
        position.clear_fields();
        holding.closed_realized_pnl += round_digits(position.realized_pnl, 2);
        holding.closed_options.insert(holding.options.extract(it));
    }
}

auto BookkeepingEngine::ensure_holding(const std::string& strategy_name)
    -> model::StrategyHolding& {
    return books_[strategy_name].holding;
}

auto BookkeepingEngine::find_holding(const std::string& strategy_name) const
    -> const model::StrategyHolding* {
    const auto it = books_.find(strategy_name);
    return it != books_.end() ? &it->second.holding : nullptr;
}

auto BookkeepingEngine::holds(const std::string& strategy_name, const std::string& symbol) const
    -> bool {
    const model::StrategyHolding* holding = find_holding(strategy_name);
    if (holding == nullptr) {
        return false;
    }
    bool held = holding->shares.quantity != 0 &&
                holding->shares.symbol == symbol;
    holding->for_each_open_option(
        [&](const model::Position& p) { held = held || p.symbol == symbol; });
    return held;
}

auto BookkeepingEngine::held_options() const -> std::vector<std::string> {
    std::set<std::string> held;
    for (const auto& [name, books] : books_) {
        books.holding.for_each_open_option(
            [&held](const model::Position& p) { held.insert(p.symbol); });
    }
    return {held.begin(), held.end()};
}

auto BookkeepingEngine::take_held_options_changed() -> bool {
    return std::exchange(held_options_changed_, false);
}

void BookkeepingEngine::value_holding(const std::string& strategy_name,
                                      const model::Portfolio& portfolio) {
    const auto found = books_.find(strategy_name);
    if (found == books_.end()) {
        return;
    }
    Books& books = found->second;
    model::StrategyHolding& holding = books.holding;

    model::HoldingSummary totals{.realized_pnl = holding.closed_realized_pnl};
    for (auto& [symbol, position] : holding.options) {
        add_totals(totals, mark_option(position, portfolio.option(symbol)));
    }

    if (holding.shares.quantity != 0 || holding.shares.realized_pnl != 0) {
        add_totals(totals,
                   mark_shares(holding.shares, shares_instrument(holding, portfolio)));
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
    attribute_pnl(books, portfolio);

    for (auto& kv : holding.options) {
        kv.second.clear_fields();
    }
    holding.shares.clear_fields();
}

void BookkeepingEngine::attribute_pnl(Books& books, const model::Portfolio& portfolio) {
    model::StrategyHolding& holding = books.holding;
    // Prices and Greeks are the portfolio's own at both ends, not the position's (rounded): a
    // rounding then cancels out from one update to the next instead of adding up.
    model::HoldingSummary& s = holding.summary;
    const model::DateTime now = portfolio.datetime();
    const std::optional<model::Underlying> u = portfolio.underlying();
    const double underlying = u ? u->mid() : 0.0;
    const std::optional<model::Underlying> instrument = shares_instrument(holding, portfolio);
    const double shares_price = instrument ? instrument->mid() : 0.0;

    AttributionStart& start = books.attribution;
    double moved = 0.0; // what the holding at the start gained by now, at mids
    if (start.time != model::DateTime{}) {
        const double move = start.underlying_price > 0.0 && underlying > 0.0
                                ? underlying - start.underlying_price
                                : 0.0;
        const double days =
            std::chrono::duration<double>(now - start.time).count() / (24.0 * 3600.0);
        for (std::size_t i = 0; i < start.options.size(); ++i) {
            const model::Position& was = start.options[i];
            const std::optional<model::Option> now_option = portfolio.option(was.symbol);
            const double mid_now = now_option ? now_option->mid() : 0.0;
            if (mid_now <= 0.0 || was.mid_price <= 0.0) {
                continue; // no mark at one end: its change comes in with pnl_fills
            }
            const double q = was.quantity;
            const double gained = q * (mid_now - was.mid_price) * was.multiplier;
            const double iv_now = now_option->iv();
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
        s.pnl_fills += s.pnl - start.pnl - moved;
    } // the first update is the baseline: earlier (rebooked) P&L is not attributed

    start.time = now;
    start.pnl = s.pnl;
    start.underlying_price = underlying;
    start.shares = holding.shares;
    start.shares.mid_price = shares_price;
    start.options.clear();
    start.option_ivs.clear();
    holding.for_each_open_option([&](const model::Position& position) {
        model::Position was = position;
        double iv = 0.0;
        if (const std::optional<model::Option> option = portfolio.option(position.symbol)) {
            was.mid_price = option->mid();
            const model::Greeks g = option->greeks();
            was.delta = g.delta;
            was.gamma = g.gamma;
            was.theta = g.theta;
            was.vega = g.vega;
            iv = option->iv();
        }
        start.options.push_back(was);
        start.option_ivs.push_back(iv);
    });
}

void BookkeepingEngine::remove_strategy(const std::string& strategy_name) {
    std::unordered_set<std::string> removed;
    std::erase_if(orders_, [&](const auto& kv) {
        if (kv.second.strategy_name != strategy_name) {
            return false;
        }
        removed.insert(kv.first);
        return true;
    });
    forget_fills(removed);
    held_options_changed_ = true;
    books_.erase(strategy_name);
}

void BookkeepingEngine::close() {
    orders_.clear();
    fills_.clear();
    fill_index_.clear();
    for (auto& [_, books] : books_) {
        books.active.clear();
        books.working.clear();
    }
}

// ---- Expiry

namespace {

/** The holding's options expired by now, by symbol. */
auto expired_in(const model::StrategyHolding& holding, model::DateTime now)
    -> std::vector<const model::OptionPosition*> {
    std::vector<const model::OptionPosition*> out;
    holding.for_each_open_option([&](const model::OptionPosition& p) {
        if (p.expires <= now) {
            out.push_back(&p);
        }
    });
    std::ranges::sort(out, {}, &model::OptionPosition::symbol);
    return out;
}

auto symbols_of(std::span<const model::OptionPosition* const> positions)
    -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const model::OptionPosition* p : positions) {
        out.push_back(p->symbol);
    }
    return out;
}

} // namespace

auto BookkeepingEngine::settle_expiry(const std::string& strategy_name,
                                      std::chrono::year_month_day expiry, double price,
                                      const model::Portfolio& portfolio) const
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

    for (const auto& [symbol, position] : holding->options) {
        const model::OptionId& id = position.id;
        if (position.quantity == 0 || position.expires == model::DateTime::max() ||
            id.expiry != expiry) {
            continue;
        }
        const double close = physical ? 0.0 : intrinsic(id, price);
        closes.push_back(single_settlement(std::format("settle-{}-{}", strategy_name, symbol),
                                           symbol, closing(position.quantity),
                                           std::abs(position.quantity), close, at));
        if (physical) {
            if (auto d = delivery(strategy_name, symbol, id, position.quantity,
                                  position.multiplier, price, underlying, at)) {
                deliveries.push_back(std::move(*d));
            }
        }
    }

    closes.insert(closes.end(), std::make_move_iterator(deliveries.begin()),
                  std::make_move_iterator(deliveries.end()));
    return closes;
}

auto BookkeepingEngine::expiries_to_settle(const std::string& strategy_name,
                                           const model::Portfolio* portfolio,
                                           model::DateTime now)
    -> std::vector<std::pair<std::chrono::year_month_day, double>> {
    const auto found = books_.find(strategy_name);
    if (found == books_.end()) {
        return {};
    }
    Books& books = found->second;
    std::map<std::chrono::year_month_day, std::vector<std::string>> by_expiry;
    for (const model::OptionPosition* p : expired_in(books.holding, now)) {
        by_expiry[p->id.expiry].push_back(p->symbol);
    }
    std::vector<std::pair<std::chrono::year_month_day, double>> due;
    std::vector<std::string> waiting;
    for (const auto& [expiry, symbols] : by_expiry) {
        if (const std::optional<double> price =
                portfolio != nullptr ? closing_price(*portfolio, expiry) : std::nullopt) {
            due.emplace_back(expiry, *price);
        } else {
            waiting.insert(waiting.end(), symbols.begin(), symbols.end());
        }
    }
    if (books.unsettled != waiting) {
        if (!waiting.empty()) {
            log_.write_log(std::format("{} holds expired options without a closing price to"
                                       " settle at ({}): SettleExpired with the underlying's close",
                                       strategy_name, utilities::join(waiting)),
                           utilities::WARNING);
        }
        books.unsettled = std::move(waiting);
    }
    return due;
}

auto BookkeepingEngine::unsettled_options() const
    -> std::unordered_map<std::string, std::vector<std::string>> {
    std::unordered_map<std::string, std::vector<std::string>> out;
    for (const auto& [name, books] : books_) {
        if (!books.unsettled.empty()) {
            out.emplace(name, books.unsettled);
        }
    }
    return out;
}

// ---- The broker's account

auto BookkeepingEngine::compare_with_broker(const std::map<std::string, double>& broker,
                                            const model::Portfolios& portfolios) const
    -> std::vector<BrokerCheckRow> {
    std::map<std::string, double> held;
    for (const auto& [name, books] : books_) {
        const model::SharePosition& shares = books.holding.shares;
        if (shares.quantity != 0 && !shares.symbol.empty()) {
            held[shares.symbol] += shares.quantity;
        }
        books.holding.for_each_open_option(
            [&held](const model::Position& p) { held[p.symbol] += p.quantity; });
    }
    std::erase_if(held, [](const auto& kv) { return std::abs(kv.second) < kQuantityTolerance; });

    std::vector<BrokerCheckRow> out;
    for (const auto& [symbol, quantity] : held) {
        const auto it = broker.find(symbol);
        out.push_back({.symbol = symbol,
                       .engine = quantity,
                       .broker = it != broker.end() ? it->second : 0.0,
                       .traded_here = true});
    }
    for (const auto& [symbol, quantity] : broker) {
        if (!held.contains(symbol)) {
            out.push_back({.symbol = symbol,
                           .broker = quantity,
                           .traded_here = portfolios.trades_contract(symbol)});
        }
    }
    std::ranges::sort(out, {}, &BrokerCheckRow::symbol);
    return out;
}

void BookkeepingEngine::record_broker_check(std::span<const BrokerCheckRow> checks) {
    std::string problem;
    for (const BrokerCheckRow& c : checks) {
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
    for (const BrokerCheckRow& c : checks) {
        if (!c.traded_here && c.broker != 0.0) {
            log_.write_log(std::format("Position in the account not traded here: {} {}", c.broker,
                                       c.symbol),
                           utilities::INFO);
        }
    }
    if (problem.empty()) {
        log_.write_log("The account's positions agree with the holdings", utilities::INFO);
    } else {
        log_.write_log("The account's positions disagree with the holdings: " + problem,
                       utilities::ERROR);
    }
}

void BookkeepingEngine::record_broker_check_failed(const std::string& reason) {
    broker_check_ = BrokerCheck::Failed;
    broker_check_problem_ = reason;
    log_.write_log("The account's positions were not checked against the holdings: " + reason,
                   utilities::ERROR);
}

auto BookkeepingEngine::broker_check() const -> std::pair<std::string, std::string> {
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

auto BookkeepingEngine::start_refusal(const std::string& strategy_name, model::DateTime now,
                                      bool broker_agreement_required) const
    -> std::optional<std::string> {
    const model::StrategyHolding* holding = find_holding(strategy_name);
    if (const std::vector<const model::OptionPosition*> expired =
            holding != nullptr ? expired_in(*holding, now)
                               : std::vector<const model::OptionPosition*>{};
        !expired.empty()) {
        return std::format(
            "{} holds expired options not settled ({}): settle them with SettleExpired",
            strategy_name, utilities::join(symbols_of(expired)));
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
                           " CheckWithBroker once the broker answers",
                           broker_check_problem_);
    case BrokerCheck::Disagree:
        return std::format("The account's positions disagree with the holdings ({}): set them"
                           " right, then CheckWithBroker",
                           broker_check_problem_);
    case BrokerCheck::Agree:
        break;
    }
    return std::nullopt;
}

} // namespace core
