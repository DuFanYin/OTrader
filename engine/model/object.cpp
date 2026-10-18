#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace model {

namespace {

/** A snapshot's values for the listing: each option's columns, then the chains' (forward), the
 * expiries' (time, rate). */
auto values_size(const Listing* listing) -> std::size_t {
    if (listing == nullptr) {
        return 0;
    }
    return (listing->size() * PortfolioSnapshot::kOptionColumns.size()) +
           listing->chains().size() + (2 * listing->expiries().size());
}

auto is_combo_of(std::span<const Leg> legs) -> bool {
    return legs.size() != 1 || std::abs(legs.front().ratio) != 1;
}

} // namespace

// ---- RateCurve

auto RateCurve::at(double t) const -> double {
    if (years.empty() || years.size() != rates.size()) {
        return 0.0;
    }
    if (t <= years.front()) {
        return rates.front();
    }
    if (t >= years.back()) {
        return rates.back();
    }
    const auto after = std::ranges::upper_bound(years, t);
    const auto j = static_cast<std::size_t>(after - years.begin());
    const double w = (t - years[j - 1]) / (years[j] - years[j - 1]);
    return rates[j - 1] + (w * (rates[j] - rates[j - 1]));
}

auto RateCurve::flat(double rate) -> RateCurve { return {.years = {0.0}, .rates = {rate}}; }

// ---- PortfolioSnapshot

PortfolioSnapshot::PortfolioSnapshot(std::shared_ptr<const Listing> listing,
                                     std::string portfolio_name)
    : owner_(std::move(listing)), name_(std::move(portfolio_name)),
      values_(values_size(owner_.get()), 0.0) {
    point();
}

PortfolioSnapshot::PortfolioSnapshot(const PortfolioSnapshot& other)
    : Frame(other), quoted_tsc(other.quoted_tsc), owner_(other.owner_), name_(other.name_),
      values_(other.values_) {
    point();
}

PortfolioSnapshot::PortfolioSnapshot(PortfolioSnapshot&& other) noexcept
    : Frame(other), quoted_tsc(other.quoted_tsc), owner_(std::move(other.owner_)),
      name_(std::move(other.name_)), values_(std::move(other.values_)) {
    point();
    other.point();
}

auto PortfolioSnapshot::operator=(const PortfolioSnapshot& other) -> PortfolioSnapshot& {
    if (this != &other) {
        Frame::operator=(other);
        quoted_tsc = other.quoted_tsc;
        owner_ = other.owner_;
        name_ = other.name_;
        values_ = other.values_; // into the buffer it has
        point();
    }
    return *this;
}

auto PortfolioSnapshot::operator=(PortfolioSnapshot&& other) noexcept -> PortfolioSnapshot& {
    if (this != &other) {
        Frame::operator=(other);
        quoted_tsc = other.quoted_tsc;
        owner_ = std::move(other.owner_);
        name_ = std::move(other.name_);
        values_ = std::move(other.values_);
        point();
        other.point();
    }
    return *this;
}

auto PortfolioSnapshot::values(std::span<const double> Frame::* column) -> std::span<double> {
    const std::span<const double> view = this->*column;
    return {const_cast<double*>(view.data()), view.size()}; // a view of values_, its own
}

void PortfolioSnapshot::assign(const Frame& frame) {
    if (&frame == this) {
        return;
    }
    static_cast<Frame&>(*this) = frame;
    values_.assign(values_size(owner_.get()), 0.0);
    point();
    const auto copy = [this](std::span<const double> from, std::span<const double> Frame::* to) {
        std::ranges::copy(from.first(std::min(from.size(), (this->*to).size())),
                          values(to).begin());
    };
    for (const auto column : kOptionColumns) {
        copy(frame.*column, column);
    }
    copy(frame.forward, &Frame::forward);
    copy(frame.expiry_years, &Frame::expiry_years);
    copy(frame.expiry_rate, &Frame::expiry_rate);
    if (frame.priced && !frame.iv_price_mode.empty()) { // its Greeks priced now, to hold them
        const std::span<double> delta = values(&Frame::delta);
        const std::span<double> gamma = values(&Frame::gamma);
        const std::span<double> theta = values(&Frame::theta);
        const std::span<double> vega = values(&Frame::vega);
        for (std::size_t c = 0; c < delta.size(); ++c) {
            const Greeks g = greeks_at(frame, c);
            delta[c] = g.delta;
            gamma[c] = g.gamma;
            theta[c] = g.theta;
            vega[c] = g.vega;
        }
    }
    iv_price_mode = {}; // its Greeks are its own: nothing to price them from, nor a view to keep
}

void PortfolioSnapshot::point() {
    listing = owner_.get();
    portfolio_name = name_;
    const std::size_t n = listing != nullptr ? listing->size() : 0;
    const std::size_t chains = listing != nullptr ? listing->chains().size() : 0;
    const std::size_t expiries = listing != nullptr ? listing->expiries().size() : 0;
    const double* at = values_.data();
    for (const auto column : kOptionColumns) {
        this->*column = {at, n};
        at += n;
    }
    forward = {at, chains};
    expiry_years = {at + chains, expiries};
    expiry_rate = {at + chains + expiries, expiries};
}

// ---- Positions

OptionPosition::OptionPosition(const std::string& sym) : Position{sym} {
    multiplier = 100.0;
    if (std::optional<OptionId> parsed = parse_option_symbol(sym)) {
        id = std::move(*parsed);
        expires = option_expiry_time(id.expiry);
    }
}

// ---- Orders and fills

auto OrderRequest::is_combo() const -> bool { return is_combo_of(legs); }

auto OrderRequest::create_order(const std::string& order_id) const -> Order {
    return {.symbol = symbol,
            .order_id = order_id,
            .trading_class = trading_class,
            .type = type,
            .price = price,
            .quantity = quantity,
            .reference = reference,
            .legs = legs,
            .combo_type = combo_type};
}

auto Order::is_active() const -> bool { return is_active_status(status); }

auto Order::is_combo() const -> bool { return is_combo_of(legs); }

auto Order::create_cancel_request() const -> CancelRequest {
    return {.order_id = order_id, .symbol = symbol};
}

auto filled_order(const std::string& order_id, const std::string& symbol, Side side,
                  double quantity, double price, DateTime at, std::string reference)
    -> std::pair<Order, Fill> {
    Order order{.symbol = symbol,
                    .order_id = order_id,
                    .type = OrderType::MARKET,
                    .price = side == Side::BUY ? price : -price,
                    .quantity = quantity,
                    .filled = quantity,
                    .status = Status::FILLED,
                    .datetime = at,
                    .reference = std::move(reference),
                    .legs = {{.ratio = 1, .side = side, .symbol = symbol}}};
    Fill fill{.symbol = symbol,
                   .order_id = order_id,
                   .fill_id = order_id,
                   .side = side,
                   .price = price,
                   .quantity = quantity,
                   .datetime = at};
    return {std::move(order), std::move(fill)};
}

// ---- Positions

auto Position::current_value() const -> double { return quantity * mid_price * multiplier; }

void Position::clear_fields() {
    if (quantity != 0) {
        return;
    }
    avg_cost = 0.0;
    cost_value = 0.0;
    mid_price = 0.0;
    delta = 0.0;
    gamma = 0.0;
    theta = 0.0;
    vega = 0.0;
}

auto StrategyHolding::max_loss() const -> double {
    struct Leg {
        std::chrono::year_month_day expiry;
        bool call = true;
        double strike = 0.0;
        double units = 0.0; // contracts x multiplier: bought +, sold -
    };
    std::vector<Leg> legs;
    double paid = 0.0;
    for_each_open_option([&](const OptionPosition& p) {
        if (const std::optional<OptionId> id = parse_option_symbol(p.symbol)) {
            legs.push_back({id->expiry, id->type == OptionType::CALL, id->strike,
                            p.quantity * p.multiplier});
            paid += p.quantity * p.avg_cost * p.multiplier;
        }
    });
    const double none = std::numeric_limits<double>::quiet_NaN();
    if (legs.empty()) {
        return none;
    }
    const bool one_expiry = std::ranges::all_of(
        legs, [&](const Leg& l) { return l.expiry == legs.front().expiry; });
    double calls_net = 0.0; // the slope past the last strike
    for (const Leg& l : legs) {
        calls_net += l.call ? l.units : 0.0;
    }
    if (one_expiry && calls_net >= 0.0) {
        const auto value = [&](double s) {
            double v = 0.0;
            for (const Leg& l : legs) {
                v += l.units * (l.call ? std::max(s - l.strike, 0.0) : std::max(l.strike - s, 0.0));
            }
            return v;
        };
        double worst = value(0.0);
        for (const Leg& l : legs) {
            worst = std::min(worst, value(l.strike));
        }
        const double loss = paid - worst;
        return loss > 0.0 ? loss : none;
    }
    return paid > 0.0 ? paid : none;
}

} // namespace model
