#include "core/risk/risk_engine.hpp"

#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>
#include <map>
#include <set>
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

constexpr double kOptionMultiplier = 100.0;

// The names the storage keeps the limits (the prefix, then the limit's name) and the halt under.
constexpr std::string_view kSavedLimit = "risk.limit.";
constexpr std::string_view kSavedHalted = "risk.halted";
constexpr std::string_view kSavedHaltReason = "risk.halt_reason";
constexpr std::string_view kSavedHaltedAt = "risk.halted_at";
constexpr std::string_view kSavedDay = "risk.day";             // the New York day the starts are of
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

auto is_option(std::string_view symbol) -> bool {
    return model::parse_option_symbol(symbol).has_value();
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
    out.emplace_back(kSavedDay, day_.ok() ? std::format("{:%F}", std::chrono::sys_days{day_})
                                          : std::string{});
    for (const auto& [name, e] : exposure_) {
        if (e.day_start) {
            out.emplace_back(std::format("{}{}", kSavedDayStart, name),
                             std::format("{}", *e.day_start));
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
    // The day's starts, so a restart keeps counting the day's loss from the morning.
    std::istringstream day_text(text(kSavedDay));
    std::chrono::sys_days day{};
    if (day_text >> std::chrono::parse("%F", day)) {
        day_ = std::chrono::year_month_day{day};
        for (const auto& [name, value] : saved) {
            if (name.starts_with(kSavedDayStart)) {
                try {
                    exposure_[name.substr(kSavedDayStart.size())].day_start = std::stod(value);
                } catch (const std::exception&) {
                }
            }
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
            std::ranges::find_if(out, [&](const Line& l) { return *l.symbol == symbol; });
        if (it != out.end()) {
            return &*it;
        }
        return add ? &out.emplace_back(Line{.symbol = &symbol}) : nullptr;
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

auto RiskEngine::portfolio_of(const std::string& portfolio, const std::string& symbol) const
    -> const model::Portfolio* {
    return !portfolio.empty() ? portfolios_.find_portfolio(portfolio)
                              : portfolios_.portfolio_of(symbol);
}

auto RiskEngine::quote(const model::Portfolio* portfolio, const std::string& symbol) const
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
    const double minutes = limits_.flatten_minutes;
    if (minutes <= 0.0 || portfolio == nullptr || !portfolio->physical) {
        return false;
    }
    const std::optional<model::OptionId> id = model::parse_option_symbol(option_symbol);
    if (!id || id->expiry != model::new_york_date(now)) {
        return false;
    }
    const auto close = model::option_expiry_time(id->expiry);
    return now >= close - std::chrono::minutes(static_cast<int>(minutes));
}

auto RiskEngine::positions_to_flatten(const std::string& strategy_name,
                                      const model::Portfolio& portfolio,
                                      model::DateTime now) const
    -> std::map<std::string, std::vector<model::OptionPosition>> {
    const model::StrategyHolding* holding = bookkeeping_.find_holding(strategy_name);
    if (limits_.flatten_minutes <= 0.0 || holding == nullptr) {
        return {};
    }
    std::set<std::string> active;
    bookkeeping_.for_each_active_order(strategy_name, [&](const model::Order& o) {
        for (const model::Leg& leg : o.legs) {
            active.insert(leg.symbol);
        }
    });
    // The day before an ex-dividend day, in the window before today's close: combos with a call
    // sold.
    std::set<std::string> before_dividend;
    const auto today = model::new_york_date(now);
    if (daily_ != nullptr && portfolio.physical &&
        daily_->next(portfolio.underlying_symbol + "_ex_dividend",
                     std::chrono::sys_days{today} + std::chrono::days{1}) ==
            model::next_trading_day(today) &&
        now >= model::new_york_time(today, model::session_close(today)) -
                   std::chrono::minutes(static_cast<int>(limits_.flatten_minutes))) {
        holding->for_each_open_option([&](const model::OptionPosition& p) {
            const std::optional<model::OptionId> id = model::parse_option_symbol(p.symbol);
            if (id && id->type == model::OptionType::CALL && p.quantity < 0) {
                before_dividend.insert(p.combo.empty() ? p.symbol : p.combo);
            }
        });
    }
    return holding->open_options_by_combo([&](const model::OptionPosition& p) {
        return !active.contains(p.symbol) &&
               (in_flatten_window(p.symbol, &portfolio, now) ||
                before_dividend.contains(p.combo.empty() ? p.symbol : p.combo));
    });
}

auto RiskEngine::refusal(const std::string& strategy_name, const std::string& portfolio,
                         const model::OrderRequest& req, model::DateTime now)
    -> std::optional<std::string> {
    const Limits& l = limits_;
    const bool sells_option = std::ranges::any_of(req.legs, [](const model::Leg& leg) {
        return leg.side == model::Side::SELL && is_option(leg.symbol);
    });
    // The whole book only for the checks that look past the order's contracts.
    const bool whole = (l.short_options < 1.0 && sells_option) || l.max_strategy_positions > 0.0 ||
                       l.max_margin > 0.0;
    const std::size_t ordered = book(strategy_name, bookkeeping_.find_holding(strategy_name), &req,
                                     whole, lines_);
    if (ordered == 0 || req.quantity <= 0.0) {
        return "nothing to fill";
    }
    const std::span<const Line> order(lines_.data(), ordered);
    // Only closing: every contract moves toward zero, and no further than it.
    bool reduces = !strategy_name.empty();
    for (const Line& c : order) {
        reduces = reduces && c.held * c.change < 0.0 && std::abs(c.change) <= std::abs(c.held);
    }
    if (halted_ && !reduces) {
        return std::format("trading is halted ({}): only closing orders go", halt_reason_);
    }

    const model::Portfolio* where = portfolio_of(portfolio, *order.front().symbol);
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
    double premium = 0.0; // USD paid for options per unit, at the market
    double net_mid = 0.0;
    bool mids = true;
    for (const Line& c : order) {
        const std::string& symbol = *c.symbol;
        const bool option = is_option(symbol);
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
        if (option && std::abs(after) > std::abs(before) && in_flatten_window(symbol, where, now)) {
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
        if (std::optional<std::string> why = uncovered(quantities(lines_, true))) {
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
        const double paid = per_unit * req.quantity * kOptionMultiplier;
        if (premium > 0.0 && paid > l.max_order_premium) {
            return std::format("it pays {:.0f} USD for options: over max_order_premium {:.0f}",
                               paid, l.max_order_premium);
        }
    }
    return std::nullopt;
}

auto RiskEngine::quantities(std::span<const Line> book, bool after)
    -> std::span<const std::pair<std::string_view, double>> {
    quantities_.clear();
    for (const Line& c : book) {
        quantities_.emplace_back(*c.symbol, after ? c.held + c.active + c.change : c.held);
    }
    return quantities_;
}

void RiskEngine::start_bar(model::DateTime now) {
    if (day_begins_ <= now && now < day_ends_) {
        return; // within the New York day found last
    }
    const std::chrono::year_month_day today = model::new_york_date(now);
    using namespace std::chrono_literals;
    day_begins_ = model::new_york_time(today, 0min);
    day_ends_ = model::new_york_time(
        std::chrono::year_month_day{std::chrono::sys_days{today} + std::chrono::days{1}}, 0min);
    if (today == day_) {
        return;
    }
    day_ = today;
    day_changed_ = true;
    for (auto& [name, e] : exposure_) {
        e.day_start.reset();
    }
    day_stops_.clear();
    bookkeeping_.for_each_holding([&](const std::string& name, const model::StrategyHolding& h) {
        exposure_[name].day_start = h.summary.pnl;
    });
}

auto RiskEngine::trading_day() const -> bool {
    // A closed day moves only the marks (quotes refreshed, or none yet): nothing to stop.
    return !day_.ok() || model::is_trading_day(day_);
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
    e.margin = margin(quantities(lines_, false), portfolio);
    const double start = e.day_start ? *e.day_start : *(e.day_start = h->summary.pnl);
    const double loss = start - h->summary.pnl;
    const double limit = limits_.max_strategy_daily_loss;
    if (limit > 0.0 && loss >= limit && trading_day() && !day_stops_.contains(strategy_name)) {
        day_stops_[strategy_name] =
            std::format("{} lost {:.0f} USD today: over max_strategy_daily_loss {:.0f}",
                        strategy_name, loss, limit);
        log_.write_log(day_stops_[strategy_name] + "; only closing orders until tomorrow",
                       utilities::ERROR);
    }
}

auto RiskEngine::day_loss() const -> std::optional<std::string> {
    const double limit = limits_.max_daily_loss;
    if (limit <= 0.0 || halted_ || !trading_day()) {
        return std::nullopt;
    }
    double loss = 0.0;
    bookkeeping_.for_each_holding([&](const std::string& name, const model::StrategyHolding& h) {
        if (const auto it = exposure_.find(name); it != exposure_.end() && it->second.day_start) {
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
    const double spot = spot_of(portfolio);
    struct Leg {
        model::OptionType type;
        double strike;
        double contracts; // shares: quantity times the multiplier, bought +
    };
    std::map<std::chrono::year_month_day, std::vector<Leg>> expiries;
    double total = 0.0;
    for (const auto& [symbol, quantity] : holding) {
        if (quantity == 0.0) {
            continue;
        }
        if (const std::optional<model::OptionId> id = model::parse_option_symbol(symbol)) {
            const std::optional<model::Option> option = portfolio.option(symbol);
            const double multiplier = option ? option->multiplier() : kOptionMultiplier;
            expiries[id->expiry].push_back({id->type, id->strike, quantity * multiplier});
        } else {
            total += std::abs(quantity) * spot * kShareMargin;
        }
    }
    for (const auto& [expiry, legs] : expiries) {
        const auto value = [&](double s) {
            double v = 0.0;
            for (const Leg& leg : legs) {
                const double intrinsic = leg.type == model::OptionType::CALL
                                             ? std::max(s - leg.strike, 0.0)
                                             : std::max(leg.strike - s, 0.0);
                v += leg.contracts * intrinsic;
            }
            return v;
        };
        double worst = value(0.0);
        double calls = 0.0; // past the last strike, the value's slope
        for (const Leg& leg : legs) {
            worst = std::min(worst, value(leg.strike));
            calls += leg.type == model::OptionType::CALL ? leg.contracts : 0.0;
        }
        total += std::max(-worst, 0.0) + std::max(-calls, 0.0) * spot * kUncoveredCall;
    }
    return total;
}

auto RiskEngine::uncovered(std::span<const std::pair<std::string_view, double>> holding)
    -> std::optional<std::string> {
    std::map<std::pair<std::chrono::year_month_day, model::OptionType>, double> net;
    for (const auto& [symbol, quantity] : holding) {
        if (const std::optional<model::OptionId> id = model::parse_option_symbol(symbol)) {
            net[{id->expiry, id->type}] += quantity;
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
        if (const std::optional<model::Option> option = portfolio.option(*c.symbol)) {
            const model::Greeks g = option->greeks();
            added.delta += c.change * g.delta * spot;
            added.gamma += c.change * g.gamma * spot * spot / 100.0;
            added.vega += c.change * g.vega;
        } else if (!is_option(*c.symbol)) {
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
    const auto after = quantities(lines_, true);
    if (max_positions > 0.0) {
        const auto options = std::ranges::count_if(after, [](const auto& p) {
            return p.second != 0.0 && is_option(p.first);
        });
        if (static_cast<double>(options) > max_positions) {
            return std::format("{} would hold {} options with its working orders: over "
                               "max_strategy_positions {}",
                               strategy_name, options, max_positions);
        }
    }
    if (max_margin > 0.0) {
        const double needed = margin(after, portfolio);
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
