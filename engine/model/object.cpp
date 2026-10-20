#include "model/object.hpp"
#include "model/daily_table.hpp"
#include "model/expiry.hpp"
#include "model/portfolio.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace model {

namespace {

/** A snapshot's values for the listing: each option's columns, then the chains' (forward), the
 * expiries' (time, rate), the chains' smiles. */
auto values_size(const Listing* listing) -> std::size_t {
    if (listing == nullptr) {
        return 0;
    }
    return (listing->size() * PortfolioSnapshot::kOptionColumns.size()) +
           (listing->chains().size() * (1 + kSmileValues)) + (2 * listing->expiries().size());
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

auto RateCurve::treasury(const DailyTable& daily, std::chrono::year_month_day day)
    -> std::optional<RateCurve> {
    static constexpr std::array<std::pair<std::string_view, double>, 5> kMaturities{
        {{"DGS1MO", 1.0 / 12.0}, {"DGS3MO", 0.25}, {"DGS6MO", 0.5}, {"DGS1", 1.0}, {"DGS2", 2.0}}};
    RateCurve curve{.years = {}, .rates = {}};
    for (const auto& [series, years] : kMaturities) {
        const std::vector<double> last = daily.before(series, day, 1);
        if (last.empty()) {
            return std::nullopt;
        }
        curve.years.push_back(years);
        curve.rates.push_back(2.0 * std::log1p(last.front() / 200.0));
    }
    return curve;
}

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
    copy(frame.smile, &Frame::smile);
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

void PortfolioSnapshot::relist(std::shared_ptr<const Listing> listing,
                               std::span<const std::uint32_t> written) {
    if (owner_ != nullptr) {
        for (const std::uint32_t c : written) {
            const Listing::Chain& chain = owner_->chains()[c];
            for (const auto column : kOptionColumns) {
                std::ranges::fill(values(column).subspan(chain.first, chain.count), 0.0);
            }
            values(&Frame::forward)[c] = 0.0;
            std::ranges::fill(values(&Frame::smile).subspan(c * kSmileValues, kSmileValues), 0.0);
        }
        std::ranges::fill(values(&Frame::expiry_years), 0.0);
        std::ranges::fill(values(&Frame::expiry_rate), 0.0);
    }
    static_cast<Frame&>(*this) = Frame{};
    quoted_tsc = 0;
    owner_ = std::move(listing);
    values_.resize(values_size(owner_.get()), 0.0); // what it grows by is zeros too
    point();
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
    smile = {at + chains + (2 * expiries), chains * kSmileValues};
}

// ---- Positions

OptionPosition::OptionPosition(const std::string& sym, double contract_multiplier)
    : Position{sym} {
    multiplier = contract_multiplier;
    if (std::optional<OptionId> parsed = parse_option_symbol(sym)) {
        id = std::move(*parsed);
        expires = calendar_of(id.root).expiry_time(id.expiry);
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
    std::vector<ExpiryLeg> legs;
    std::chrono::year_month_day expiry{};
    bool one_expiry = true;
    double paid = 0.0;
    for_each_open_option([&](const OptionPosition& p) {
        if (const std::optional<OptionId> id = parse_option_symbol(p.symbol)) {
            one_expiry = one_expiry && (legs.empty() || id->expiry == expiry);
            expiry = id->expiry;
            legs.push_back({id->type, id->strike, p.quantity * p.multiplier});
            paid += p.quantity * p.avg_cost * p.multiplier;
        }
    });
    const double none = std::numeric_limits<double>::quiet_NaN();
    if (legs.empty()) {
        return none;
    }
    if (const ExpiryWorst w = worst_at_expiry(legs); one_expiry && w.calls_net >= 0.0) {
        const double loss = paid - w.worst;
        return loss > 0.0 ? loss : none;
    }
    return paid > 0.0 ? paid : none;
}

} // namespace model
