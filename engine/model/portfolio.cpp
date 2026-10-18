/** Portfolio, Listing, pricing and the reads. */

#include "model/portfolio.hpp"
#include "model/symbol.hpp"

#include "quant/kernel.h"
#include "utilities/thread_pool.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <compare>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

namespace model {

namespace {

/** The underlying's mark from its quotes, else as marked. */
auto frame_spot(const Frame& frame) -> double {
    return mark(frame.underlying_bid, frame.underlying_ask, frame.underlying_mark);
}

enum class IvInput : std::uint8_t { Mid, Bid, Ask };

auto iv_input(std::string_view mode) -> IvInput {
    if (mode == "bid") {
        return IvInput::Bid;
    }
    return mode == "ask" ? IvInput::Ask : IvInput::Mid;
}

auto iv_input_price(double bid, double ask, IvInput input) -> double {
    if (bid > 0.0 && ask > 0.0 && bid > ask) {
        return 0.0; // crossed: no valid quote
    }
    switch (input) {
    case IvInput::Bid:
        return bid > 0.0 ? bid : 0.0;
    case IvInput::Ask:
        return ask > 0.0 ? ask : 0.0;
    case IvInput::Mid:
        break;
    }
    return mark(bid, ask);
}

/** Parity's median over the three strikes nearest the money quoted both ways; 0 for none (its
 * options are priced on the spot grown, pricing_forward). */
auto chain_forward(const Listing::Chain& chain, const Frame& frame, double spot, double rate)
    -> double {
    if (spot <= 0.0) {
        return 0.0;
    }
    const double t = frame.expiry_years[chain.expiry_index];
    if (t <= 0.0) {
        return 0.0;
    }
    // C - P = e^(-rt) (F - K)
    struct Estimate {
        double gap; // |C - P|: smallest at the strikes nearest the forward
        double forward;
    };
    thread_local std::vector<Estimate> estimates;
    thread_local std::vector<double> forwards;
    estimates.clear();
    const double grow = std::exp(rate * t);
    for (std::size_t j = 0; j < chain.strikes.size(); ++j) {
        if (chain.calls[j] < 0 || chain.puts[j] < 0) {
            continue;
        }
        const auto c = static_cast<std::size_t>(chain.calls[j]);
        const auto p = static_cast<std::size_t>(chain.puts[j]);
        if (!(frame.bid[c] > 0.0 && frame.ask[c] >= frame.bid[c] && frame.bid[p] > 0.0 &&
              frame.ask[p] >= frame.bid[p])) {
            continue;
        }
        const double gap = mark(frame.bid[c], frame.ask[c]) - mark(frame.bid[p], frame.ask[p]);
        estimates.push_back({std::abs(gap), chain.strikes[j] + (grow * gap)});
    }
    std::optional<double> implied;
    if (!estimates.empty()) {
        // A median: one stale quote does not move it.
        const auto nearest = std::min<std::ptrdiff_t>(3, std::ssize(estimates));
        std::ranges::partial_sort(estimates, estimates.begin() + nearest, {}, &Estimate::gap);
        forwards.clear();
        for (const Estimate& e : estimates | std::views::take(nearest)) {
            forwards.push_back(e.forward);
        }
        std::ranges::sort(forwards);
        const std::size_t mid = forwards.size() / 2;
        implied = forwards.size() % 2 == 1 ? forwards[mid]
                                           : 0.5 * (forwards[mid - 1] + forwards[mid]);
    }
    return implied && std::abs((*implied / spot) - 1.0) < Chain::kMaxForwardGap ? *implied : 0.0;
}

/** An option with no spot, strike, time left or price to price it from: no IV, no Greeks. */
auto cannot_price(double spot, double strike, double years, double price) -> bool {
    return spot <= 0.0 || strike <= 0.0 || years <= 0.0 || price <= 0.0;
}

/** The forward an option is priced on: its chain's by parity, else the spot grown at the rate. */
auto pricing_forward(double chain_forward, double spot, double rate, double years) -> double {
    return chain_forward <= 0.0 ? spot * std::exp(rate * years) : chain_forward;
}

auto value_at(std::span<const double> values, std::size_t i) -> double {
    return i < values.size() ? values[i] : 0.0;
}

void copy_time_and_underlying_quotes(const Frame& from, Frame& to) {
    to.datetime = from.datetime;
    to.underlying_bid = from.underlying_bid;
    to.underlying_ask = from.underlying_ask;
    to.underlying_mark = from.underlying_mark;
    to.hedge_bid = from.hedge_bid;
    to.hedge_ask = from.hedge_ask;
    to.hedge_mark = from.hedge_mark;
}

} // namespace

// ---- Listing

namespace {

/** An option as the listing orders it: by chain symbol (root_YYYYMMDD), then by its symbol. */
struct Listed {
    OptionId id;
    std::string_view symbol;
    std::size_t given = 0; // its place among the contracts given
};

/** The order of the chain symbols root_YYYYMMDD, without writing them. */
auto chain_order(const OptionId& a, const OptionId& b) -> std::weak_ordering {
    if (a.root != b.root) {
        const std::size_t n = std::min(a.root.size(), b.root.size());
        if (const int c = a.root.compare(0, n, b.root, 0, n); c != 0) {
            return c <=> 0;
        }
        const char after_a = a.root.size() > n ? a.root[n] : '_';
        const char after_b = b.root.size() > n ? b.root[n] : '_';
        return after_a <=> after_b;
    }
    return a.expiry <=> b.expiry; // YYYYMMDD: as the text sorts
}

auto listed_before(const Listed& a, const Listed& b) -> bool {
    const std::weak_ordering chain = chain_order(a.id, b.id);
    return chain != 0 ? chain < 0 : a.symbol < b.symbol;
}

} // namespace

Listing::Listing(std::vector<Contract> contracts) {
    std::vector<Listed> listed;
    listed.reserve(contracts.size());
    for (std::size_t k = 0; k < contracts.size(); ++k) {
        if (std::optional<OptionId> id = parse_option_symbol(contracts[k].symbol)) {
            listed.push_back({.id = std::move(*id), .symbol = contracts[k].symbol, .given = k});
        }
    }
    if (!std::ranges::is_sorted(listed, listed_before)) {
        std::ranges::stable_sort(listed, listed_before); // one given twice: the last given last
    }
    contracts_.reserve(listed.size());
    strikes_.reserve(listed.size());
    types_.reserve(listed.size());
    chain_of_.reserve(listed.size());
    const OptionId* chain_id = nullptr; // the chain being listed: its first option's
    for (std::size_t k = 0; k < listed.size(); ++k) {
        if (k + 1 < listed.size() && listed[k + 1].symbol == listed[k].symbol) {
            continue; // given again later
        }
        const Listed& option = listed[k];
        if (chain_id == nullptr || chain_order(*chain_id, option.id) != 0) {
            chain_id = &option.id;
            Chain& chain = chains_.emplace_back();
            chain.symbol = chain_symbol(option.id.root, option.id.expiry);
            chain.expiry = option_expiry_time(option.id.expiry);
            chain.first = static_cast<std::uint32_t>(contracts_.size());
            const auto known = std::ranges::find(expiries_, chain.expiry);
            chain.expiry_index = static_cast<std::uint32_t>(known - expiries_.begin());
            if (known == expiries_.end()) {
                expiries_.push_back(chain.expiry);
            }
        }
        ++chains_.back().count;
        contracts_.push_back(std::move(contracts[option.given]));
        strikes_.push_back(option.id.strike);
        types_.push_back(option.id.type);
        chain_of_.push_back(static_cast<std::uint32_t>(chains_.size() - 1));
    }
    for (Chain& chain : chains_) {
        const auto strikes = std::span(strikes_).subspan(chain.first, chain.count);
        chain.strikes.assign(strikes.begin(), strikes.end());
        std::ranges::sort(chain.strikes);
        chain.strikes.erase(std::ranges::unique(chain.strikes).begin(), chain.strikes.end());
        chain.calls.assign(chain.strikes.size(), -1);
        chain.puts.assign(chain.strikes.size(), -1);
        for (std::uint32_t column = chain.first; column < chain.first + chain.count; ++column) {
            const auto at = std::ranges::lower_bound(chain.strikes, strikes_[column]) -
                            chain.strikes.begin();
            (types_[column] == OptionType::CALL ? chain.calls : chain.puts)[at] =
                static_cast<std::int32_t>(column);
        }
    }
    columns_.assign(std::bit_ceil(2 * contracts_.size() + 1), 0);
    const std::size_t mask = columns_.size() - 1;
    for (std::uint32_t column = 0; column < contracts_.size(); ++column) {
        std::size_t at = std::hash<std::string_view>{}(contracts_[column].symbol) & mask;
        for (; columns_[at] != 0; at = (at + 1) & mask) {
        }
        columns_[at] = column + 1;
    }
}

auto Listing::column(std::string_view symbol) const -> std::optional<std::uint32_t> {
    if (columns_.empty()) {
        return std::nullopt;
    }
    const std::size_t mask = columns_.size() - 1;
    for (std::size_t at = std::hash<std::string_view>{}(symbol) & mask; columns_[at] != 0;
         at = (at + 1) & mask) {
        if (contracts_[columns_[at] - 1].symbol == symbol) {
            return columns_[at] - 1;
        }
    }
    return std::nullopt;
}

auto Listing::chain(std::string_view symbol) const -> std::optional<std::uint32_t> {
    const auto by_symbol = [](const Chain& c) -> std::string_view { return c.symbol; };
    const auto it = std::ranges::lower_bound(chains_, symbol, {}, by_symbol);
    return it != chains_.end() && it->symbol == symbol
               ? std::optional(static_cast<std::uint32_t>(it - chains_.begin()))
               : std::nullopt;
}

// ---- Pricing

auto iv_price_mode_of(std::string_view text) -> std::string_view {
    for (const std::string_view mode : {"mid", "bid", "ask"}) {
        if (std::ranges::equal(text, mode, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == b;
            })) {
            return mode;
        }
    }
    return {};
}

void price_snapshot(PortfolioSnapshot& snapshot, const RateCurve& rates,
                    std::string_view iv_price_mode, std::span<const std::uint8_t> quotes_changed,
                    bool underlying_changed, std::chrono::seconds time_step) {
    if (snapshot.listing == nullptr) {
        snapshot.priced = false;
        return;
    }
    const Listing& listing = *snapshot.listing;
    const std::size_t n = listing.size();
    const std::span<const Listing::Chain> chains = listing.chains();
    const std::span<const DateTime> expiries = listing.expiries();
    const double spot = frame_spot(snapshot);
    const bool again_only =
        !quotes_changed.empty() && quotes_changed.size() == n && snapshot.priced && spot > 0.0;
    constexpr double kSecondsAYear = 365.25 * 24.0 * 3600.0;
    const double step_years =
        again_only ? static_cast<double>(time_step.count()) / kSecondsAYear : 0.0;
    const double min_left_years =
        static_cast<double>(std::chrono::seconds(Portfolio::kTimeStepMinTimeLeft).count()) /
        kSecondsAYear;
    const bool rescale = again_only && underlying_changed && snapshot.greeks_spot > 0.0;
    const double spot_ratio = rescale ? snapshot.greeks_spot / spot : 1.0;
    const std::span<double> iv = snapshot.values(&Frame::iv);
    const std::span<double> delta = snapshot.values(&Frame::delta);
    const std::span<double> gamma = snapshot.values(&Frame::gamma);
    const std::span<double> theta = snapshot.values(&Frame::theta);
    const std::span<double> vega = snapshot.values(&Frame::vega);
    const std::span<double> forward = snapshot.values(&Frame::forward);
    const std::span<double> expiry_years = snapshot.values(&Frame::expiry_years);
    const std::span<double> expiry_rate = snapshot.values(&Frame::expiry_rate);
    if (!again_only) {
        for (const std::span<double> values :
             {iv, delta, gamma, theta, vega, forward, expiry_years, expiry_rate}) {
            std::ranges::fill(values, 0.0);
        }
    }

    thread_local std::vector<std::uint8_t> expiry_moved;
    thread_local std::vector<std::uint8_t> chain_moved;
    expiry_moved.assign(expiries.size(), 1);
    chain_moved.assign(chains.size(), 1);
    bool any_expiry_moved = false;
    for (std::size_t e = 0; e < expiries.size(); ++e) {
        const double t = years_to_expiry(snapshot.datetime, expiries[e]);
        const bool within_step = std::abs(t - expiry_years[e]) < step_years &&
                                 t > min_left_years && expiry_years[e] > min_left_years;
        const bool moved = !again_only || (t != expiry_years[e] && !within_step);
        expiry_moved[e] = static_cast<std::uint8_t>(moved);
        any_expiry_moved = any_expiry_moved || moved;
        if (moved) {
            expiry_years[e] = t;
            expiry_rate[e] = rates.at(t);
        }
    }
    for (std::size_t c = 0; c < chains.size(); ++c) {
        const Listing::Chain& chain = chains[c];
        // Its quotes and time as before, and its parity forward still near the spot: the same.
        if (again_only && expiry_moved[chain.expiry_index] == 0 && forward[c] > 0.0 &&
            std::abs((forward[c] / spot) - 1.0) < Chain::kMaxForwardGap &&
            std::ranges::none_of(quotes_changed.subspan(chain.first, chain.count),
                                 [](std::uint8_t q) { return q != 0; })) {
            chain_moved[c] = 0;
            continue;
        }
        const double f = chain_forward(chain, snapshot, spot, expiry_rate[chain.expiry_index]);
        chain_moved[c] = static_cast<std::uint8_t>(!again_only || f != forward[c]);
        forward[c] = f;
    }

    const IvInput input = iv_input(iv_price_mode);
    // Spans: the pool's threads would see their own (empty) thread_locals.
    const std::span<const std::uint8_t> expiry_flags = expiry_moved;
    const std::span<const std::uint8_t> chain_flags = chain_moved;
    auto solve = [&](std::size_t i, bool again) {
        const std::uint32_t c = listing.chain_of(i);
        const std::uint32_t e = chains[c].expiry_index;
        const double k = listing.strike(i);
        const double t = expiry_years[e];
        const double r = expiry_rate[e];
        const bool is_call = listing.type(i) == OptionType::CALL;
        const double px = iv_input_price(snapshot.bid[i], snapshot.ask[i], input);
        if (cannot_price(spot, k, t, px)) {
            iv[i] = delta[i] = gamma[i] = theta[i] = vega[i] = 0.0;
            return;
        }
        const double f = pricing_forward(forward[c], spot, r, t);
        const double vol = again ? quant::black_implied_vol(px, f, k, t, r, is_call) : iv[i];
        const quant::BlackGreeks g = quant::black_greeks(is_call, spot, f, k, t, r, vol);
        iv[i] = vol;
        delta[i] = g.delta;
        gamma[i] = g.gamma;
        theta[i] = g.theta;
        vega[i] = g.vega;
    };
    // An expiry moved: the whole portfolio to the pool, as a whole pricing goes (what moved alone
    // can be under the pool's threshold and run here, one solve after another).
    if (!again_only || (underlying_changed && !rescale) || any_expiry_moved) {
        utilities::shared_pool().parallel_for(0, n, [&](std::size_t i) {
            const std::uint32_t c = listing.chain_of(i);
            const bool again = !again_only || quotes_changed[i] != 0 ||
                               expiry_flags[chains[c].expiry_index] != 0 || chain_flags[c] != 0 ||
                               (forward[c] <= 0.0 && underlying_changed);
            if (!again && !underlying_changed) {
                return; // nothing it is priced from moved
            }
            if (!again && rescale) { // per unit of spot: delta once, gamma twice
                delta[i] *= spot_ratio;
                gamma[i] *= spot_ratio * spot_ratio;
                return;
            }
            solve(i, again);
        });
    } else {
        if (rescale) { // every option's, then what moved solved again
            const double gamma_ratio = spot_ratio * spot_ratio;
            for (std::size_t i = 0; i < n; ++i) {
                delta[i] *= spot_ratio;
                gamma[i] *= gamma_ratio;
            }
        }
        thread_local std::vector<std::uint32_t> moved;
        moved.clear();
        for (std::size_t c = 0; c < chains.size(); ++c) {
            const Listing::Chain& chain = chains[c];
            const bool whole = chain_flags[c] != 0 || (forward[c] <= 0.0 && underlying_changed);
            for (std::uint32_t i = chain.first; i < chain.first + chain.count; ++i) {
                if (whole || quotes_changed[i] != 0) {
                    moved.push_back(i);
                }
            }
        }
        for (const std::uint32_t i : moved) {
            solve(i, true);
        }
    }
    snapshot.priced = true;
    snapshot.greeks_spot = spot;
    snapshot.iv_price_mode = {}; // it holds its Greeks now
}

auto greeks_at(const Frame& frame, std::size_t column) -> Greeks {
    if (frame.iv_price_mode.empty()) { // it holds them
        return {.delta = value_at(frame.delta, column),
                .gamma = value_at(frame.gamma, column),
                .theta = value_at(frame.theta, column),
                .vega = value_at(frame.vega, column)};
    }
    if (!frame.priced || frame.listing == nullptr || column >= frame.listing->size()) {
        return {};
    }
    const Listing& listing = *frame.listing;
    const std::uint32_t c = listing.chain_of(column);
    const double spot = frame.greeks_spot;
    const double k = listing.strike(column);
    const std::uint32_t e = listing.chains()[c].expiry_index;
    const double t = frame.expiry_years[e];
    const double px = iv_input_price(frame.bid[column], frame.ask[column],
                                     iv_input(frame.iv_price_mode));
    if (cannot_price(spot, k, t, px)) {
        return {};
    }
    const double r = value_at(frame.expiry_rate, e);
    const double f = pricing_forward(frame.forward[c], spot, r, t);
    const quant::BlackGreeks g = quant::black_greeks(listing.type(column) == OptionType::CALL, spot,
                                                     f, k, t, r, frame.iv[column]);
    return {.delta = g.delta, .gamma = g.gamma, .theta = g.theta, .vega = g.vega};
}

// ---- Chain, Option, Underlying

Chain::Chain(const Portfolio& portfolio, std::uint32_t index)
    : portfolio_(&portfolio), index_(index) {}

auto Chain::call(double strike) const -> std::optional<Option> {
    return option(OptionType::CALL, strike);
}

auto Chain::put(double strike) const -> std::optional<Option> {
    return option(OptionType::PUT, strike);
}

auto Chain::option(OptionType type, double strike) const -> std::optional<Option> {
    const Listing::Chain& chain = listed();
    const auto it = std::ranges::lower_bound(chain.strikes, strike);
    if (it == chain.strikes.end() || *it != strike) {
        return std::nullopt;
    }
    const std::int32_t column =
        (type == OptionType::CALL ? chain.calls : chain.puts)[it - chain.strikes.begin()];
    if (column < 0) {
        return std::nullopt;
    }
    return Option(*portfolio_, static_cast<std::uint32_t>(column));
}

auto Chain::days_to_expiry() const -> int {
    const auto hours =
        std::chrono::duration_cast<std::chrono::hours>(expiry() - portfolio_->trading_day())
            .count();
    return hours > 0 ? static_cast<int>(hours / 24) : 0;
}

auto Chain::forward() const -> double {
    const Frame& f = portfolio_->frame();
    if (const double parity = value_at(f.forward, index_); parity > 0.0) {
        return parity;
    }
    if (!f.priced || f.listing == nullptr || index_ >= f.listing->chains().size()) {
        return 0.0;
    }
    // None by parity: the spot grown, as its options are priced.
    const std::uint32_t e = f.listing->chains()[index_].expiry_index;
    const double spot = frame_spot(f);
    const double t = value_at(f.expiry_years, e);
    return spot > 0.0 && t > 0.0 ? pricing_forward(0.0, spot, value_at(f.expiry_rate, e), t)
                                 : 0.0;
}

auto Chain::rate() const -> double {
    const Frame& f = portfolio_->frame();
    return f.listing != nullptr && index_ < f.listing->chains().size()
               ? value_at(f.expiry_rate, f.listing->chains()[index_].expiry_index)
               : 0.0;
}

auto Chain::reference_price() const -> double {
    if (const double f = forward(); f > 0.0) {
        return f;
    }
    const std::optional<Underlying> u = portfolio_->underlying();
    return u ? u->mid() : 0.0;
}

auto Chain::atm_strike() const -> double {
    const std::span<const double> listed_strikes = strikes();
    if (listed_strikes.empty()) {
        return 0.0;
    }
    const double reference = reference_price();
    if (reference <= 0.0) {
        return listed_strikes[listed_strikes.size() / 2];
    }
    return *std::ranges::min_element(listed_strikes, {}, [reference](double strike) {
        return std::abs(strike - reference);
    });
}

auto Chain::skew_25d() const -> std::optional<double> {
    std::array<std::vector<double>, 2> deltas, ivs; // calls, puts
    for (const double strike : strikes()) {
        for (const OptionType type : {OptionType::CALL, OptionType::PUT}) {
            if (const std::optional<Option> o = option(type, strike)) {
                const std::size_t side = type == OptionType::CALL ? 0 : 1;
                deltas[side].push_back(o->delta());
                ivs[side].push_back(o->iv());
            }
        }
    }
    const double call = quant::iv_at_delta(deltas[0], ivs[0], 0.25);
    const double put = quant::iv_at_delta(deltas[1], ivs[1], 0.25);
    return std::isfinite(call) && std::isfinite(put) ? std::optional(call - put) : std::nullopt;
}

auto Chain::term_slope(const Chain& far) const -> std::optional<double> {
    const std::optional<double> near_iv = atm_iv();
    const std::optional<double> far_iv = far.atm_iv();
    return near_iv && far_iv ? std::optional(*far_iv - *near_iv) : std::nullopt;
}

auto Chain::atm_iv() const -> std::optional<double> {
    const double atm = atm_strike();
    if (atm <= 0.0) {
        return std::nullopt;
    }
    for (const OptionType type : {OptionType::CALL, OptionType::PUT}) {
        if (const std::optional<Option> o = option(type, atm); o && o->iv() != 0.0) {
            return o->iv();
        }
    }
    return std::nullopt;
}

auto Chain::listed() const -> const Listing::Chain& {
    return portfolio_->listing().chains()[index_];
}

Option::Option(const Portfolio& portfolio, std::uint32_t column)
    : portfolio_(&portfolio), column_(column) {}

auto Option::symbol() const -> const std::string& {
    return portfolio_->listing().contract(column_).symbol;
}

auto Option::strike() const -> double { return portfolio_->listing().strike(column_); }

auto Option::type() const -> OptionType { return portfolio_->listing().type(column_); }

auto Option::expiry() const -> DateTime { return chain().expiry(); }

auto Option::multiplier() const -> double {
    return portfolio_->listing().contract(column_).multiplier;
}

auto Option::chain() const -> Chain {
    return {*portfolio_, portfolio_->listing().chain_of(column_)};
}

auto Option::bid() const -> double { return value(&Frame::bid); }
auto Option::ask() const -> double { return value(&Frame::ask); }
auto Option::mid() const -> double { return mark(bid(), ask()); }
auto Option::bid_size() const -> double { return value(&Frame::bid_size); }
auto Option::ask_size() const -> double { return value(&Frame::ask_size); }
auto Option::iv() const -> double { return value(&Frame::iv); }
auto Option::delta() const -> double { return greeks().delta; }
auto Option::gamma() const -> double { return greeks().gamma; }
auto Option::theta() const -> double { return greeks().theta; }
auto Option::vega() const -> double { return greeks().vega; }

auto Option::value(std::span<const double> Frame::* values) const -> double {
    return value_at(portfolio_->frame().*values, column_);
}

auto Option::greeks() const -> Greeks {
    const double m = multiplier();
    const double times = m != 0.0 ? m : 1.0;
    const Greeks g = greeks_at(portfolio_->frame(), column_);
    return {.delta = g.delta * times,
            .gamma = g.gamma * times,
            .theta = g.theta * times,
            .vega = g.vega * times};
}

auto Underlying::symbol() const -> const std::string& { return contract().symbol; }

auto Underlying::multiplier() const -> double { return contract().multiplier; }

auto Underlying::bid() const -> double {
    const Frame& f = portfolio_->frame();
    return hedge_ ? f.hedge_bid : f.underlying_bid;
}

auto Underlying::ask() const -> double {
    const Frame& f = portfolio_->frame();
    return hedge_ ? f.hedge_ask : f.underlying_ask;
}

auto Underlying::mid() const -> double {
    const Frame& f = portfolio_->frame();
    return hedge_ ? f.hedge_mark : f.underlying_mark;
}

auto Underlying::theo_delta() const -> double {
    if (!hedge_) {
        return multiplier();
    }
    const double spot = frame_spot(portfolio_->frame());
    const double price = mid();
    return price > 0.0 && spot > 0.0 ? price * multiplier() / spot : 0.0;
}

auto Underlying::contract() const -> const Contract& {
    return hedge_ ? *portfolio_->hedge_instrument_ : *portfolio_->underlying_;
}

// ---- Portfolio

Portfolio::Portfolio(std::string name_)
    : name(std::move(name_)), listing_(std::make_shared<const Listing>()), own_(listing_, name) {
    read_own();
}

void Portfolio::set_risk_free_rate(double rate) {
    if (std::isfinite(rate)) {
        rates_ = RateCurve::flat(rate);
    }
}

void Portfolio::set_rates(RateCurve rates) { rates_ = std::move(rates); }

void Portfolio::set_iv_price_mode(std::string_view mode) {
    if (const std::string_view known = iv_price_mode_of(mode); !known.empty()) {
        iv_price_mode_ = known;
    }
}

void Portfolio::set_underlying(const Contract& contract) {
    underlying_ = contract;
    underlying_symbol = contract.symbol;
}

void Portfolio::set_hedge_instrument(const Contract& contract) {
    hedge_instrument_ = contract;
    hedge_symbol = contract.symbol;
}

auto Portfolio::replace_options(std::span<const Contract> options,
                                const std::unordered_set<std::string>& keep)
    -> std::vector<std::string> {
    std::unordered_set<std::string_view> today;
    for (const Contract& contract : options) {
        today.insert(contract.symbol);
    }
    std::vector<Contract> listed;
    for (const Contract& contract : listing_->contracts()) {
        if (!today.contains(contract.symbol) && keep.contains(contract.symbol)) {
            listed.push_back(contract);
        }
    }
    listed.append_range(options);
    return list(std::make_shared<const Listing>(std::move(listed)));
}

auto Portfolio::list(std::shared_ptr<const Listing> listing) -> std::vector<std::string> {
    if (listing == listing_) {
        return {};
    }
    // Both listings' chains in order, and each chain's strikes: each found in the one before by
    // walking the two.
    std::vector<std::string> dropped; // in symbol order, as the chains are listed
    std::vector<std::optional<std::uint32_t>> chain_from(listing->chains().size());
    std::vector<std::optional<std::uint32_t>> from(listing->size());
    const std::span<const Listing::Chain> before = listing_->chains();
    std::size_t b = 0;
    for (std::size_t c = 0; c < chain_from.size(); ++c) {
        const Listing::Chain& now = listing->chains()[c];
        for (; b < before.size() && before[b].symbol < now.symbol; ++b) {
            dropped.push_back(before[b].symbol);
        }
        if (b == before.size() || before[b].symbol != now.symbol) {
            continue;
        }
        chain_from[c] = static_cast<std::uint32_t>(b);
        const Listing::Chain& was = before[b];
        std::size_t k = 0;
        for (std::size_t j = 0; j < now.strikes.size(); ++j) {
            for (; k < was.strikes.size() && was.strikes[k] < now.strikes[j]; ++k) {
            }
            if (k == was.strikes.size() || was.strikes[k] != now.strikes[j]) {
                continue;
            }
            for (const auto side : {&Listing::Chain::calls, &Listing::Chain::puts}) {
                if ((now.*side)[j] >= 0 && (was.*side)[k] >= 0) {
                    from[static_cast<std::size_t>((now.*side)[j])] =
                        static_cast<std::uint32_t>((was.*side)[k]);
                }
            }
        }
        ++b;
    }
    for (; b < before.size(); ++b) {
        dropped.push_back(before[b].symbol);
    }
    PortfolioSnapshot carried(listing, name);
    const Frame& was = frame_;
    copy_time_and_underlying_quotes(was, carried);
    carried.priced = was.priced;
    carried.greeks_spot = was.greeks_spot;
    for (const auto column : PortfolioSnapshot::kOptionColumns) {
        const std::span<double> to = carried.values(column);
        for (std::size_t i = 0; i < to.size(); ++i) {
            if (from[i]) {
                to[i] = value_at(was.*column, *from[i]);
            }
        }
    }
    const std::span<double> forward = carried.values(&Frame::forward);
    for (std::size_t c = 0; c < forward.size(); ++c) {
        if (chain_from[c]) {
            forward[c] = value_at(was.forward, *chain_from[c]);
        }
    }
    const std::span<double> years = carried.values(&Frame::expiry_years);
    const std::span<double> rates = carried.values(&Frame::expiry_rate);
    const std::span<const DateTime> expiries = listing_->expiries();
    for (std::size_t e = 0; e < years.size(); ++e) {
        const auto it = std::ranges::find(expiries, listing->expiries()[e]);
        if (it != expiries.end()) {
            const auto before_e = static_cast<std::size_t>(it - expiries.begin());
            years[e] = value_at(was.expiry_years, before_e);
            rates[e] = value_at(was.expiry_rate, before_e);
        }
    }
    listing_ = std::move(listing);
    own_ = std::move(carried);
    read_own();
    return dropped;
}

auto Portfolio::snapshot() const -> PortfolioSnapshot { return {listing_, name}; }

void Portfolio::apply(PortfolioSnapshot& snapshot) {
    if (snapshot.listing != listing_.get()) {
        apply(static_cast<const Frame&>(snapshot));
        return;
    }
    if (!snapshot.priced) {
        price_snapshot(snapshot);
    }
    std::swap(own_, snapshot);
    read_own();
    move_time(frame_.datetime);
}

void Portfolio::apply(const Frame& frame, std::shared_ptr<const void> storage) {
    if (frame.listing != listing_.get()) {
        if (storage_ != nullptr) {
            own_.assign(frame_);
            read_own();
        }
        copy_time_and_underlying_quotes(frame, own_);
        read_own();
    } else if (!frame.priced) {
        own_.assign(frame);
        price_snapshot(own_);
        read_own();
    } else if (storage != nullptr) {
        frame_ = frame;
        storage_ = std::move(storage);
    } else {
        own_.assign(frame);
        read_own();
    }
    move_time(frame.datetime);
}

void Portfolio::price_snapshot(PortfolioSnapshot& snapshot,
                               std::span<const std::uint8_t> quotes_changed,
                               bool underlying_changed, std::chrono::seconds time_step) const {
    model::price_snapshot(snapshot, rates_, iv_price_mode_, quotes_changed,
                          underlying_changed, time_step);
}

auto Portfolio::underlying() const -> std::optional<Underlying> {
    if (!underlying_) {
        return std::nullopt;
    }
    return Underlying(*this, false);
}

auto Portfolio::hedge() const -> std::optional<Underlying> {
    if (hedge_symbol.empty()) {
        return underlying();
    }
    if (!hedge_instrument_) {
        return std::nullopt;
    }
    return Underlying(*this, true);
}

auto Portfolio::instrument(std::string_view symbol) const -> std::optional<Underlying> {
    if (underlying_ && underlying_->symbol == symbol) {
        return Underlying(*this, false);
    }
    if (hedge_instrument_ && hedge_instrument_->symbol == symbol) {
        return Underlying(*this, true);
    }
    return std::nullopt;
}

auto Portfolio::option(std::string_view symbol) const -> std::optional<Option> {
    const std::optional<std::uint32_t> column = listing_->column(symbol);
    if (!column) {
        return std::nullopt;
    }
    return Option(*this, *column);
}

auto Portfolio::chain(std::string_view symbol) const -> std::optional<Chain> {
    const std::optional<std::uint32_t> index = listing_->chain(symbol);
    if (!index) {
        return std::nullopt;
    }
    return Chain(*this, *index);
}

auto Portfolio::contract(std::string_view symbol) const -> const Contract* {
    if (const std::optional<std::uint32_t> column = listing_->column(symbol)) {
        return &listing_->contract(*column);
    }
    if (underlying_ && underlying_->symbol == symbol) {
        return &*underlying_;
    }
    if (hedge_instrument_ && hedge_instrument_->symbol == symbol) {
        return &*hedge_instrument_;
    }
    return nullptr;
}

auto Portfolio::chains_by_expiry(int min_dte, int max_dte) const -> std::vector<std::string> {
    std::vector<std::string> out;
    for (std::uint32_t c = 0; c < listing_->chains().size(); ++c) {
        const int dte = Chain(*this, c).days_to_expiry();
        if (min_dte <= dte && dte <= max_dte) {
            out.push_back(listing_->chains()[c].symbol);
        }
    }
    return out;
}

void Portfolio::read_own() {
    frame_ = own_;
    storage_.reset();
}

void Portfolio::move_time(DateTime time) {
    if (time == DateTime{}) {
        return;
    }
    datetime_ = time;
    dte_ref_ = std::chrono::floor<std::chrono::days>(time);
}

} // namespace model
