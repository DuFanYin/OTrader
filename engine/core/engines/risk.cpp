#include "core/engines/risk.hpp"

#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <stdexcept>

namespace core {

namespace {

// name, default, min, max, integer. The defaults leave a backtest as it was.
constexpr std::array kParameters{
    strategy::StrategyParam{"max_option_order", 100, 1, 1'000'000, true},
    strategy::StrategyParam{"max_share_order", 10'000, 1, 100'000'000, true},
    strategy::StrategyParam{"max_option_position", 500, 1, 10'000'000, true},
    strategy::StrategyParam{"max_share_position", 100'000, 1, 1'000'000'000, true},
    strategy::StrategyParam{"max_order_premium", 0, 0, 1e9, false},
    strategy::StrategyParam{"price_band", 0, 0, 10, false},
    strategy::StrategyParam{"price_band_floor", 0.1, 0, 1000, false},
    strategy::StrategyParam{"market_max_spread", 0, 0, 10, false},
    strategy::StrategyParam{"short_options", 1, 0, 1, true},
    strategy::StrategyParam{"flatten_minutes", 0, 0, 390, true},
};

constexpr double kOptionMultiplier = 100.0;

auto sign(model::Direction direction) -> double {
    return direction == model::Direction::LONG ? 1.0 : -1.0;
}

auto is_option(const std::string& symbol) -> bool {
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

auto RiskEngine::parameters() -> std::span<const strategy::StrategyParam> { return kParameters; }

RiskEngine::RiskEngine(utilities::Logger* logger, const model::PortfolioStructure& portfolios,
                       const PositionEngine& positions, const ExecutionEngine& execution)
    : BaseEngine(logger, "Risk"), portfolios_(portfolios), positions_(positions),
      execution_(execution) {
    for (const strategy::StrategyParam& p : kParameters) {
        settings_[std::string(p.name)] = p.default_value;
    }
}

auto RiskEngine::setting(std::string_view name) const -> double {
    const auto it = settings_.find(std::string(name));
    return it != settings_.end() ? it->second : 0.0;
}

void RiskEngine::set(const std::unordered_map<std::string, double>& values) {
    for (const auto& [name, value] : values) {
        const auto* p = std::ranges::find(kParameters, name, &strategy::StrategyParam::name);
        if (p == kParameters.end()) {
            throw std::invalid_argument("No risk limit " + name);
        }
        if (!std::isfinite(value) || value < p->min || value > p->max ||
            (p->integer && value != std::floor(value))) {
            throw std::invalid_argument(std::format("Risk limit {} must be {} from {} to {}", name,
                                                    p->integer ? "a whole number" : "a number",
                                                    p->min, p->max));
        }
    }
    for (const auto& [name, value] : values) {
        settings_[name] = value;
    }
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

auto RiskEngine::changes(const model::OrderRequest& req)
    -> std::vector<std::pair<std::string, double>> {
    std::vector<std::pair<std::string, double>> out;
    if (req.is_combo) {
        // Each leg carries its own side (IB buys a BAG, its legs saying which way).
        for (const model::Leg& leg : req.legs) {
            if (!leg.symbol.empty()) {
                out.emplace_back(leg.symbol, sign(leg.direction) *
                                                  std::abs(static_cast<double>(leg.ratio)) *
                                                  req.volume);
            }
        }
        return out;
    }
    out.emplace_back(req.symbol, sign(req.direction) * req.volume);
    return out;
}

void RiskEngine::held_and_working(const std::string& strategy_name,
                                  std::unordered_map<std::string, double>& held,
                                  std::unordered_map<std::string, double>& working) {
    if (const model::StrategyHolding* h = positions_.find_holding(strategy_name)) {
        h->for_each_open_option(
            [&held](const model::BasePosition& p) { held[p.symbol] += p.quantity; });
        if (h->underlyingPosition.quantity != 0) {
            held[h->underlyingPosition.symbol] += h->underlyingPosition.quantity;
        }
    }
    for (const std::string& orderid : execution_.active_orders(strategy_name)) {
        const model::OrderData* order = execution_.get_order(orderid);
        if (order == nullptr || !order->is_active()) {
            continue;
        }
        const double remaining = std::max(order->volume - order->traded, 0.0);
        if (order->is_combo) {
            for (const model::Leg& leg : order->legs) {
                if (!leg.symbol.empty()) {
                    working[leg.symbol] += sign(leg.direction) *
                                            std::abs(static_cast<double>(leg.ratio)) * remaining;
                }
            }
        } else {
            working[order->symbol] += sign(order->direction) * remaining;
        }
    }
}

auto RiskEngine::portfolio_of(const std::string& portfolio, const std::string& symbol) const
    -> const model::PortfolioData* {
    return !portfolio.empty() ? portfolios_.get_portfolio(portfolio)
                              : portfolios_.portfolio_of(symbol);
}

auto RiskEngine::quote(const model::PortfolioData* portfolio, const std::string& symbol) const
    -> std::optional<std::array<double, 3>> {
    if (portfolio == nullptr) {
        return std::nullopt;
    }
    if (const auto it = portfolio->options.find(symbol); it != portfolio->options.end()) {
        const model::OptionData& o = it->second;
        return std::array{o.bid_price, o.ask_price, o.mid_price};
    }
    for (const model::UnderlyingData* u :
         {portfolio->underlying.get(), portfolio->hedge_instrument.get()}) {
        if (u != nullptr && u->symbol == symbol) {
            return std::array{u->bid_price, u->ask_price, u->mid_price};
        }
    }
    return std::nullopt;
}

auto RiskEngine::in_flatten_window(const std::string& option_symbol,
                                   const model::PortfolioData* portfolio,
                                   model::DateTime now) const -> bool {
    const double minutes = setting("flatten_minutes");
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

auto RiskEngine::refusal(const std::string& strategy_name, const std::string& portfolio,
                         const model::OrderRequest& req, model::DateTime now)
    -> std::optional<std::string> {
    const std::vector<std::pair<std::string, double>> order = changes(req);
    if (order.empty() || req.volume <= 0.0) {
        return "nothing to trade";
    }
    std::unordered_map<std::string, double> held;
    std::unordered_map<std::string, double> working;
    if (!strategy_name.empty()) {
        held_and_working(strategy_name, held, working);
    }
    // Only closing: every contract moves toward zero, and no further than it.
    const bool reduces = !strategy_name.empty() && std::ranges::all_of(order, [&](const auto& c) {
        const double h = held[c.first];
        return h * c.second < 0.0 && std::abs(c.second) <= std::abs(h);
    });
    if (halted_ && !reduces) {
        return std::format("trading is halted ({}): only closing orders go", halt_reason_);
    }

    const model::PortfolioData* where =
        portfolio_of(portfolio, order.front().first);
    double premium = 0.0; // USD paid for options per unit, at the market
    double net_mid = 0.0;
    bool mids = true;
    for (const auto& [symbol, quantity] : order) {
        const bool option = is_option(symbol);
        const double max_order = setting(option ? "max_option_order" : "max_share_order");
        if (std::abs(quantity) > max_order) {
            return std::format("{} {} in one order: over {} {}", std::abs(quantity), symbol,
                               option ? "max_option_order" : "max_share_order", max_order);
        }
        const std::optional<std::array<double, 3>> q = quote(where, symbol);
        const double bid = q ? (*q)[0] : 0.0;
        const double ask = q ? (*q)[1] : 0.0;
        const double mid = q ? (*q)[2] : 0.0;
        if (req.type == model::OrderType::MARKET && setting("market_max_spread") > 0.0) {
            if (bid <= 0.0 || ask <= 0.0) {
                return std::format("a market order with no two-sided quote for {}", symbol);
            }
            if ((ask - bid) > setting("market_max_spread") * mid) {
                return std::format("a market order with {} quoted {}/{}: spread over "
                                   "market_max_spread",
                                   symbol, bid, ask);
            }
        }
        const double per_unit = std::abs(quantity) / req.volume;
        net_mid += (quantity > 0 ? 1.0 : -1.0) * per_unit * mid;
        mids = mids && mid > 0.0;
        if (option) {
            premium += (quantity > 0 ? 1.0 : -1.0) * per_unit * (quantity > 0 ? ask : bid);
        }

        const double before = held[symbol] + working[symbol];
        const double after = before + quantity;
        if (option && setting("short_options") < 1.0 && !strategy_name.empty() && after < 0.0) {
            return std::format("it would leave {} short {} (short_options 0)", strategy_name,
                               symbol);
        }
        const double max_position = setting(option ? "max_option_position" : "max_share_position");
        if (std::abs(after) > max_position && std::abs(after) > std::abs(before)) {
            return std::format("{} would hold {} {} with its working orders: over {} {}",
                               strategy_name.empty() ? "the order" : strategy_name, after, symbol,
                               option ? "max_option_position" : "max_share_position",
                               max_position);
        }
        if (option && std::abs(after) > std::abs(before) && in_flatten_window(symbol, where, now)) {
            return std::format("{} expires at the close: only closing it from {} minutes before",
                               symbol, setting("flatten_minutes"));
        }
    }

    if (req.type == model::OrderType::LIMIT && setting("price_band") > 0.0 && mids) {
        const double gap = std::abs(std::abs(req.price) - std::abs(net_mid));
        const double allowed =
            std::max(setting("price_band") * std::abs(net_mid), setting("price_band_floor"));
        if (gap > allowed) {
            return std::format("limit {} is {:.2f} from the mid {:.2f}: over price_band", req.price,
                               gap, std::abs(net_mid));
        }
    }
    if (setting("max_order_premium") > 0.0) {
        const double per_unit = req.type == model::OrderType::LIMIT && premium > 0.0
                                    ? std::abs(req.price)
                                    : premium;
        const double paid = per_unit * req.volume * kOptionMultiplier;
        if (premium > 0.0 && paid > setting("max_order_premium")) {
            return std::format("it pays {:.0f} USD for options: over max_order_premium {:.0f}",
                               paid, setting("max_order_premium"));
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
    write_log("Order refused: " + line, utilities::WARNING);
    return false;
}

} // namespace core
