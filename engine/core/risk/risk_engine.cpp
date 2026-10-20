#include "core/risk/risk_engine.hpp"

#include "model/expiry.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace core {

namespace {


/** The underlying's mid; 0 before its contract is loaded. */
auto spot_of(const model::Portfolio& portfolio) -> double {
    const std::optional<model::Underlying> u = portfolio.underlying();
    return u ? u->mid() : 0.0;
}

// The names the storage keeps the limits (the prefix, then the limit's name) and the halt under.
constexpr std::string_view kSavedLimit = "risk.limit.";
constexpr std::string_view kSavedHalted = "risk.halted";
constexpr std::string_view kSavedHaltReason = "risk.halt_reason";
constexpr std::string_view kSavedHaltedAt = "risk.halted_at";
// Saved before calendars: the one day the starts were of.
constexpr std::string_view kSavedDayStart = "risk.day_start."; // then the strategy's name
constexpr double kUncoveredCall = 0.2;   // of the underlying, for each call sold uncovered
constexpr double kShareMargin = 0.5;     // of the shares' value

auto sign(model::Side side) -> double {
    return side == model::Side::BUY ? 1.0 : -1.0;
}

/** Signed quantity per contract: each leg's side, times its ratio. */
template <typename Order, typename Fn>
void for_each_change(const Order& order, double quantity, Fn&& fn) {
    for (const model::Leg& leg : order.legs) {
        if (!leg.symbol.empty()) {
            fn(leg.symbol, sign(leg.side) * std::abs(static_cast<double>(leg.ratio)) * quantity);
        }
    }
}

/** The New York time of day, for a refusal's line. */
auto clock_text(model::DateTime t) -> std::string {
    try {
        const std::chrono::zoned_time local{"America/New_York",
                                            std::chrono::floor<std::chrono::seconds>(t)};
        return std::format("{:%H:%M:%S}", local);
    } catch (const std::exception&) {
        return std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::seconds>(t));
    }
}

} // namespace

auto RiskEngine::parameters() -> std::span<const model::Parameter> {
    static const std::vector<model::Parameter> parameters = model::parameters_of<Limits>();
    return parameters;
}

RiskEngine::RiskEngine(utilities::Logger* logger, const model::Portfolios& portfolios,
                       const BookkeepingEngine& book)
    : log_(logger, "Risk"), portfolios_(portfolios), bookkeeping_(book),
      limit_values_(model::resolve(parameters(), {}, "Risk limits")),
      limits_(model::read<Limits>(limit_values_)) {}

auto RiskEngine::limit(std::string_view name) const -> double {
    const auto it = limit_values_.find(std::string(name));
    return it != limit_values_.end() ? it->second : 0.0;
}

void RiskEngine::set_limits(const std::unordered_map<std::string, double>& values) {
    model::Values changed = limit_values_;
    for (const auto& [name, value] : values) {
        changed[name] = value;
    }
    limit_values_ = model::resolve(parameters(), changed, "Risk limits");
    limits_ = model::read<Limits>(limit_values_);
}

auto RiskEngine::saved_limits() const -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    for (const model::Parameter& p : parameters()) {
        out.emplace_back(std::format("{}{}", kSavedLimit, p.name),
                         std::format("{}", limit_values_.at(std::string(p.name))));
    }
    out.emplace_back(kSavedHalted, halted_ ? "1" : "0");
    out.emplace_back(kSavedHaltReason, halt_reason_);
    out.emplace_back(kSavedHaltedAt,
                     std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                        halted_at_.time_since_epoch())
                                        .count()));
    std::ranges::move(saved_day(), std::back_inserter(out));
    return out;
}

auto RiskEngine::saved_day() const -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [name, e] : exposure_) {
        if (e.day_start && e.day.ok()) {
            out.emplace_back(std::format("{}{}", kSavedDayStart, name),
                             std::format("{:%F} {}", std::chrono::sys_days{e.day}, *e.day_start));
        }
    }
    return out;
}

void RiskEngine::restore_limits(const std::unordered_map<std::string, std::string>& saved) {
    std::unordered_map<std::string, double> limits;
    for (const auto& [name, value] : saved) {
        if (!name.starts_with(kSavedLimit)) {
            continue;
        }
        try {
            limits[name.substr(kSavedLimit.size())] = std::stod(value);
        } catch (const std::exception&) {
            log_.write_log(std::format("Saved risk limit {} is not a number: {}", name, value),
                           utilities::ERROR);
        }
    }
    // One by one: a limit renamed or re-ranged since it was saved keeps its default, not all.
    for (const auto& [name, value] : limits) {
        try {
            set_limits({{name, value}});
        } catch (const std::exception& e) {
            log_.write_log(std::format("Saved risk limit {} not used: {}", name, e.what()),
                           utilities::ERROR);
        }
    }
    const auto text = [&](std::string_view name) {
        const auto it = saved.find(std::string(name));
        return it != saved.end() ? it->second : std::string{};
    };
    // Each strategy's day and its start, so a restart keeps counting the day's loss from the
    // morning: "<date> <pnl>".
    for (const auto& [name, value] : saved) {
        if (!name.starts_with(kSavedDayStart)) {
            continue;
        }
        std::istringstream in(value);
        std::chrono::sys_days day{};
        double start = 0.0;
        if (in >> std::chrono::parse("%F", day) >> start) {
            Exposure& e = exposure_[name.substr(kSavedDayStart.size())];
            e.day = std::chrono::year_month_day{day};
            e.day_start = start;
        }
    }
    if (text(kSavedHalted) != "1") {
        return;
    }
    long long seconds = 0;
    try {
        seconds = std::stoll(text(kSavedHaltedAt));
    } catch (const std::exception&) {
    }
    halt(text(kSavedHaltReason), model::DateTime{} + std::chrono::seconds(seconds));
    log_.write_log("Trading is halted (" + halt_reason_ + "), as saved: clear the halt to fill",
                   utilities::WARNING);
}

void RiskEngine::halt(const std::string& reason, model::DateTime at) {
    halted_ = true;
    halt_reason_ = reason;
    halted_at_ = at;
}

void RiskEngine::clear_halt() {
    halted_ = false;
    halt_reason_.clear();
    halted_at_ = {};
}

auto RiskEngine::book(const std::string& strategy_name, const model::StrategyHolding* holding,
                      const model::OrderRequest* req, bool whole, std::vector<Line>& out) const
    -> std::size_t {
    out.clear();
    // A book is a few contracts: found by comparing, not hashing.
    const auto line = [&out](const std::string& symbol, bool add) -> Line* {
        const auto it =
            std::ranges::find_if(out, [&](const Line& l) { return l.symbol == symbol; });
        if (it != out.end()) {
            return &*it;
        }
        return add ? &out.emplace_back(
                         Line{.symbol = symbol, .option = model::parse_option_symbol(symbol)})
                   : nullptr;
    };
    if (req != nullptr) {
        for_each_change(*req, req->quantity, [&](const std::string& symbol, double quantity) {
            line(symbol, true)->change += quantity;
        });
    }
    const std::size_t ordered = out.size();
    if (strategy_name.empty()) {
        return ordered;
    }
    if (const model::StrategyHolding* h = holding) {
        h->for_each_open_option([&](const model::Position& p) {
            if (Line* l = line(p.symbol, whole)) {
                l->held += p.quantity;
            }
        });
        if (const model::Position& u = h->shares; u.quantity != 0) {
            if (Line* l = line(u.symbol, whole)) {
                l->held += u.quantity;
            }
        }
    }
    bookkeeping_.for_each_working(strategy_name, [&](const std::string& symbol, double quantity) {
        if (Line* l = line(symbol, whole)) {
            l->active += quantity;
        }
    });
    return ordered;
}

auto RiskEngine::portfolio_of(const std::string& portfolio, std::string_view symbol) const
    -> const model::Portfolio* {
    return !portfolio.empty() ? portfolios_.find_portfolio(portfolio)
                              : portfolios_.portfolio_of(symbol);
}

auto RiskEngine::quote(const model::Portfolio* portfolio, std::string_view symbol) const
    -> std::optional<std::array<double, 3>> {
    if (portfolio == nullptr) {
        return std::nullopt;
    }
    if (const std::optional<model::Option> o = portfolio->option(symbol)) {
        return std::array{o->bid(), o->ask(), o->mid()};
    }
    if (const std::optional<model::Underlying> u = portfolio->instrument(symbol)) {
        return std::array{u->bid(), u->ask(), u->mid()};
    }
    return std::nullopt;
}

auto RiskEngine::in_flatten_window(const std::string& option_symbol,
                                   const model::Portfolio* portfolio,
                                   model::DateTime now) const -> bool {
    const std::optional<model::OptionId> id = model::parse_option_symbol(option_symbol);
    return id && BookkeepingEngine::in_flatten_window(*id, portfolio, now, limits_.flatten_minutes);
}

auto RiskEngine::refusal(const std::string& strategy_name, const std::string& portfolio,
                         const model::OrderRequest& req, model::DateTime now)
    -> std::optional<std::string> {
    const Limits& l = limits_;
    const bool sells = std::ranges::any_of(
        req.legs, [](const model::Leg& leg) { return leg.side == model::Side::SELL; });
    // The whole book only for the checks that look past the order's contracts.
    const bool whole =
        (l.short_options < 1.0 && sells) || l.max_strategy_positions > 0.0 || l.max_margin > 0.0;
    const std::size_t ordered = book(strategy_name, bookkeeping_.find_holding(strategy_name), &req,
                                     whole, lines_);
    if (ordered == 0 || req.quantity <= 0.0) {
        return "nothing to fill";
    }
    const std::span<const Line> order(lines_.data(), ordered);
    const bool sells_option =
        std::ranges::any_of(order, [](const Line& c) { return c.option && c.change < 0.0; });
    // Only closing: every contract moves toward zero, and no further than it.
    bool reduces = !strategy_name.empty();
    for (const Line& c : order) {
        reduces = reduces && c.held * c.change < 0.0 && std::abs(c.change) <= std::abs(c.held);
    }
    if (halted_ && !reduces) {
        return std::format("trading is halted ({}): only closing orders go", halt_reason_);
    }

    const model::Portfolio* where = portfolio_of(portfolio, order.front().symbol);
    if (!reduces) {
        if (const auto stop = day_stops_.find(strategy_name); stop != day_stops_.end()) {
            return std::format("{}: only closing orders until tomorrow", stop->second);
        }
        if (broker_connected_ && !broker_connected_()) {
            return std::string("the broker is not connected: only closing orders");
        }
        if (l.max_quote_age_seconds > 0.0 && where != nullptr) {
            const auto age =
                std::chrono::duration_cast<std::chrono::seconds>(now - where->datetime());
            if (age.count() > l.max_quote_age_seconds) {
                return std::format("{}'s quotes are {} s old: over max_quote_age_seconds {}",
                                   where->name, age.count(), l.max_quote_age_seconds);
            }
        }
    }
    double premium = 0.0; // paid for options per unit, at the market, per share of a contract
    std::string_view an_option; // its multiplier makes the premium USD
    double net_mid = 0.0;
    bool mids = true;
    for (const Line& c : order) {
        const std::string_view symbol = c.symbol;
        const bool option = c.option.has_value();
        const double max_order = option ? l.max_option_order : l.max_share_order;
        if (std::abs(c.change) > max_order) {
            return std::format("{} {} in one order: over {} {}", std::abs(c.change), symbol,
                               option ? "max_option_order" : "max_share_order", max_order);
        }
        const auto [bid, ask, mid] = quote(where, symbol).value_or(std::array{0.0, 0.0, 0.0});
        if (req.type == model::OrderType::MARKET && l.market_max_spread > 0.0) {
            if (bid <= 0.0 || ask <= 0.0) {
                return std::format("a market order with no two-sided quote for {}", symbol);
            }
            if ((ask - bid) > l.market_max_spread * mid) {
                return std::format("a market order with {} quoted {}/{}: spread over "
                                   "market_max_spread",
                                   symbol, bid, ask);
            }
        }
        const double per_unit = c.change / req.quantity; // signed
        net_mid += per_unit * mid;
        mids = mids && mid > 0.0;
        if (option) {
            premium += per_unit * (per_unit > 0 ? ask : bid);
            an_option = symbol;
        }

        const double before = c.held + c.active;
        const double after = before + c.change;
        const double max_position = option ? l.max_option_position : l.max_share_position;
        if (std::abs(after) > max_position && std::abs(after) > std::abs(before)) {
            return std::format("{} would hold {} {} with its working orders: over {} {}",
                               strategy_name.empty() ? "the order" : strategy_name, after, symbol,
                               option ? "max_option_position" : "max_share_position",
                               max_position);
        }
        if (option && std::abs(after) > std::abs(before) &&
            BookkeepingEngine::in_flatten_window(*c.option, where, now, l.flatten_minutes)) {
            return std::format("{} expires at the close: only closing it from {} minutes before",
                               symbol, l.flatten_minutes);
        }
    }

    if (req.type == model::OrderType::LIMIT && l.price_band > 0.0 && mids) {
        const double gap = std::abs(req.price - net_mid); // both the net a unit, signed alike
        const double allowed =
            std::max(l.price_band * std::abs(net_mid), l.price_band_floor.value());
        if (gap > allowed) {
            return std::format("limit {} is {:.2f} from the mid {:.2f}: over price_band", req.price,
                               gap, std::abs(net_mid));
        }
    }
    if (!strategy_name.empty() && l.short_options < 1.0 && sells_option) {
        if (std::optional<std::string> why = uncovered(lines_)) {
            return std::format("{} would hold {}: uncovered (short_options 0)", strategy_name,
                               *why);
        }
    }
    if (!reduces && !strategy_name.empty() && where != nullptr) {
        if (std::optional<std::string> why = exposure_refusal(strategy_name, *where, ordered)) {
            return why;
        }
    }
    if (l.max_order_premium > 0.0) {
        const double per_unit = req.type == model::OrderType::LIMIT && premium > 0.0
                                    ? std::abs(req.price)
                                    : premium;
        const double paid =
            per_unit * req.quantity * (an_option.empty() ? 1.0 : portfolios_.multiplier(an_option));
        if (premium > 0.0 && paid > l.max_order_premium) {
            return std::format("it pays {:.0f} USD for options: over max_order_premium {:.0f}",
                               paid, l.max_order_premium);
        }
    }
    return std::nullopt;
}

void RiskEngine::start_bar(const std::string& strategy_name, const model::Portfolio& portfolio,
                           model::DateTime now) {
    const model::SessionCalendar& calendar = portfolio.calendar();
    const std::chrono::year_month_day today = calendar.date_of(now);
    Exposure& e = exposure_[strategy_name];
    if (today == e.day) {
        return;
    }
    e.day = today;
    // A closed day moves only the marks (quotes refreshed, or none yet): nothing to stop.
    e.trading_day = calendar.is_trading_day(today);
    day_changed_ = true;
    day_stops_.erase(strategy_name);
    // Where the holding was valued last; else (a position unquoted since a restart) where it is
    // first valued whole (measure_exposure).
    const model::StrategyHolding* h = bookkeeping_.find_holding(strategy_name);
    e.day_start = h != nullptr && h->valued() ? std::optional(h->summary.pnl) : std::nullopt;
}

void RiskEngine::measure_exposure(const std::string& strategy_name,
                                  const model::Portfolio& portfolio) {
    const model::StrategyHolding* h = bookkeeping_.find_holding(strategy_name);
    if (h == nullptr) {
        return;
    }
    const double spot = spot_of(portfolio);
    book(strategy_name, h, nullptr, true, lines_);
    Exposure& e = exposure_[strategy_name];
    e.delta = h->summary.delta * spot;
    e.gamma = h->summary.gamma * spot * spot / 100.0;
    e.vega = h->summary.vega;
    e.margin = margin(lines_, false, portfolio);
    if (!e.day_start) {
        if (!h->valued()) {
            return; // not from a summary that values a position at nothing
        }
        e.day_start = h->summary.pnl;
        day_changed_ = true; // saved: a restart today keeps counting from it
    }
    const double loss = *e.day_start - h->summary.pnl;
    const double limit = limits_.max_strategy_daily_loss;
    if (limit > 0.0 && loss >= limit && e.trading_day && !day_stops_.contains(strategy_name)) {
        day_stops_[strategy_name] =
            std::format("{} lost {:.0f} USD today: over max_strategy_daily_loss {:.0f}",
                        strategy_name, loss, limit);
        log_.write_log(day_stops_[strategy_name] + "; only closing orders until tomorrow",
                       utilities::ERROR);
    }
}

auto RiskEngine::day_loss() const -> std::optional<std::string> {
    const double limit = limits_.max_daily_loss;
    if (limit <= 0.0 || halted_) {
        return std::nullopt;
    }
    double loss = 0.0;
    bookkeeping_.for_each_holding([&](const std::string& name, const model::StrategyHolding& h) {
        if (const auto it = exposure_.find(name);
            it != exposure_.end() && it->second.trading_day && it->second.day_start) {
            loss += *it->second.day_start - h.summary.pnl;
        }
    });
    if (loss < limit) {
        return std::nullopt;
    }
    return std::format("the strategies lost {:.0f} USD today: over max_daily_loss {:.0f}", loss,
                       limit);
}

auto RiskEngine::margin(const std::string& strategy_name) const -> double {
    const auto it = exposure_.find(strategy_name);
    return it != exposure_.end() ? it->second.margin : 0.0;
}

auto RiskEngine::margin(std::span<const std::pair<std::string_view, double>> holding,
                        const model::Portfolio& portfolio) -> double {
    std::vector<Line> book;
    for (const auto& [symbol, quantity] : holding) {
        book.push_back({.symbol = symbol, .option = model::parse_option_symbol(symbol),
                        .held = quantity});
    }
    return margin(book, false, portfolio);
}

auto RiskEngine::margin(std::span<const Line> book, bool after, const model::Portfolio& portfolio)
    -> double {
    const double spot = spot_of(portfolio);
    std::map<std::chrono::year_month_day, std::vector<model::ExpiryLeg>> expiries;
    double total = 0.0;
    for (const Line& c : book) {
        const double quantity = after ? c.after() : c.held;
        if (quantity == 0.0) {
            continue;
        }
        if (const std::optional<model::OptionId>& id = c.option) {
            const std::optional<model::Option> option = portfolio.option(c.symbol);
            const double multiplier = option ? option->multiplier()
                                             : model::Contract::kOptionMultiplierDefault;
            expiries[id->expiry].push_back({id->type, id->strike, quantity * multiplier});
        } else {
            total += std::abs(quantity) * spot * kShareMargin;
        }
    }
    for (const auto& [expiry, legs] : expiries) {
        const model::ExpiryWorst w = model::worst_at_expiry(legs);
        total += std::max(-w.worst, 0.0) + std::max(-w.calls_net, 0.0) * spot * kUncoveredCall;
    }
    return total;
}

auto RiskEngine::uncovered(std::span<const Line> book) -> std::optional<std::string> {
    std::map<std::pair<std::chrono::year_month_day, model::OptionType>, double> net;
    for (const Line& c : book) {
        if (c.option) {
            net[{c.option->expiry, c.option->type}] += c.after();
        }
    }
    for (const auto& [kind, quantity] : net) {
        if (quantity < 0.0) {
            return std::format("{} {} expiring {:%F} sold beyond those bought", -quantity,
                               kind.second == model::OptionType::CALL ? "calls" : "puts",
                               std::chrono::sys_days{kind.first});
        }
    }
    return std::nullopt;
}

auto RiskEngine::exposure_refusal(const std::string& strategy_name,
                                  const model::Portfolio& portfolio, std::size_t ordered)
    -> std::optional<std::string> {
    const double spot = spot_of(portfolio);
    Exposure added; // the order's
    for (const Line& c : std::span(lines_).first(ordered)) {
        if (const std::optional<model::Option> option = portfolio.option(c.symbol)) {
            const model::Greeks g = option->greeks();
            added.delta += c.change * g.delta * spot;
            added.gamma += c.change * g.gamma * spot * spot / 100.0;
            added.vega += c.change * g.vega;
        } else if (!c.option) {
            added.delta += c.change * spot;
        }
    }
    const auto it = exposure_.find(strategy_name);
    const Exposure before = it != exposure_.end() ? it->second : Exposure{};
    Exposure total;
    for (const auto& [name, e] : exposure_) {
        total.delta += e.delta;
        total.gamma += e.gamma;
        total.vega += e.vega;
    }
    constexpr std::array kGreeks{"delta", "gamma", "vega"};
    constexpr std::array kExposure{&Exposure::delta, &Exposure::gamma, &Exposure::vega};
    constexpr std::array kOwn{&Limits::max_strategy_delta_usd, &Limits::max_strategy_gamma_usd,
                              &Limits::max_strategy_vega_usd};
    constexpr std::array kAll{&Limits::max_total_delta_usd, &Limits::max_total_gamma_usd,
                              &Limits::max_total_vega_usd};
    for (std::size_t g = 0; g < kGreeks.size(); ++g) {
        const double adds = added.*kExposure[g];
        const double was = before.*kExposure[g];
        const double own = limits_.*kOwn[g];
        if (own > 0.0 && std::abs(was + adds) > own && std::abs(was + adds) > std::abs(was)) {
            return std::format("{} would hold {:.0f} USD of {}: over max_strategy_{}_usd {:.0f}",
                               strategy_name, was + adds, kGreeks[g], kGreeks[g], own);
        }
        const double all = total.*kExposure[g];
        const double cap = limits_.*kAll[g];
        if (cap > 0.0 && std::abs(all + adds) > cap && std::abs(all + adds) > std::abs(all)) {
            return std::format("the strategies would hold {:.0f} USD of {}: over "
                               "max_total_{}_usd {:.0f}",
                               all + adds, kGreeks[g], kGreeks[g], cap);
        }
    }
    const double max_positions = limits_.max_strategy_positions;
    const double max_margin = limits_.max_margin;
    if (max_positions <= 0.0 && max_margin <= 0.0) {
        return std::nullopt;
    }
    if (max_positions > 0.0) {
        const auto options = std::ranges::count_if(
            lines_, [](const Line& c) { return c.after() != 0.0 && c.option; });
        if (static_cast<double>(options) > max_positions) {
            return std::format("{} would hold {} options with its working orders: over "
                               "max_strategy_positions {}",
                               strategy_name, options, max_positions);
        }
    }
    if (max_margin > 0.0) {
        const double needed = margin(lines_, true, portfolio);
        if (needed > max_margin && needed > before.margin) {
            return std::format("{} would take {:.0f} USD of margin: over max_margin {:.0f}",
                               strategy_name, needed, max_margin);
        }
    }
    return std::nullopt;
}

auto RiskEngine::allow(const std::string& strategy_name, const std::string& portfolio,
                       const model::OrderRequest& req, model::DateTime now) -> bool {
    const std::optional<std::string> why = refusal(strategy_name, portfolio, req, now);
    if (!why) {
        return true;
    }
    ++refused_;
    const std::string line = std::format("{} {} {}: {}", clock_text(now),
                                         strategy_name.empty() ? "-" : strategy_name, req.symbol,
                                         *why);
    recent_refusals_.push_back(line);
    if (recent_refusals_.size() > kRecentRefusals) {
        recent_refusals_.pop_front();
    }
    log_.write_log("Order refused: " + line, utilities::WARNING);
    return false;
}

} // namespace core
