/** Portfolio, Listing and the reads. */

#include "model/portfolio.hpp"
#include "model/symbol.hpp"

#include "quant/engine_api.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cctype>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace model {

namespace {

auto value_at(std::span<const double> values, std::size_t i) -> double {
    return i < values.size() ? values[i] : 0.0;
}

/** A chain's smile values (Frame::smile) as SVI. */
auto svi_of(std::span<const double> fitted) -> quant::Svi {
    return {.a = fitted[kSviA], .b = fitted[kSviB], .rho = fitted[kSviRho], .m = fitted[kSviM],
            .sigma = fitted[kSviSigma]};
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
            chain.expiry = calendar_of(option.id.root).expiry_time(option.id.expiry);
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

// ---- How a frame is priced

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

auto Chain::fitted() const -> std::span<const double> {
    const std::span<const double> all = portfolio_->frame().smile;
    const std::size_t at = static_cast<std::size_t>(index_) * kSmileValues;
    if (at + kSmileValues > all.size() || all[at + kSmilePoints] <= 0.0) {
        return {};
    }
    return all.subspan(at, kSmileValues);
}

auto Chain::smile() const -> quant::SviFit {
    const std::span<const double> v = fitted();
    if (v.empty()) {
        return {};
    }
    return quant::SviFit{.svi = svi_of(v),
                         .error = v[kSmileError],
                         .points = static_cast<int>(v[kSmilePoints]),
                         .butterfly_free = v[kSmileButterflyFree] != 0.0};
}

auto Chain::smile_iv(double strike) const -> std::optional<double> {
    const std::span<const double> v = fitted();
    if (v.empty() || strike <= 0.0) {
        return std::nullopt;
    }
    const double w = svi_of(v).total_variance(std::log(strike / v[kSmileForward]));
    return w > 0.0 ? std::optional(std::sqrt(w / v[kSmileYears])) : std::nullopt;
}

auto Chain::smile_iv_at_delta(double delta) const -> std::optional<double> {
    const std::span<const double> v = fitted();
    const double spot = frame_spot(portfolio_->frame());
    if (v.empty() || spot <= 0.0 || delta == 0.0 || std::abs(delta) >= 1.0) {
        return std::nullopt;
    }
    const quant::Svi svi = svi_of(v);
    const double forward = v[kSmileForward];
    const double years = v[kSmileYears];
    const double r = rate();
    const bool is_call = delta > 0.0;
    const auto delta_at = [&](double k) {
        const double w = svi.total_variance(k);
        return w > 0.0 ? quant::black_greeks(is_call, spot, forward, forward * std::exp(k), years,
                                             r, std::sqrt(w / years))
                             .delta
                       : std::numeric_limits<double>::quiet_NaN();
    };
    // A call's delta and a put's fall as the strike rises: bisect ln(strike / forward) within ten
    // ATM deviations.
    const double reach = 10.0 * std::sqrt(std::max(svi.total_variance(0.0), 1e-12));
    double lo = -reach;
    double hi = reach;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        const double d = delta_at(mid);
        if (std::isnan(d)) {
            return std::nullopt;
        }
        (d > delta ? lo : hi) = mid;
    }
    const double k = 0.5 * (lo + hi);
    if (!(std::abs(delta_at(k) - delta) < 1e-6)) {
        return std::nullopt;
    }
    return std::sqrt(svi.total_variance(k) / years);
}

auto Chain::atm_iv() const -> std::optional<double> {
    const std::span<const double> v = fitted();
    return v.empty() ? std::nullopt : smile_iv(v[kSmileForward]);
}

auto Chain::skew_25d() const -> std::optional<double> {
    const std::optional<double> call = smile_iv_at_delta(0.25);
    const std::optional<double> put = smile_iv_at_delta(-0.25);
    return call && put ? std::optional(*call - *put) : std::nullopt;
}

auto Chain::term_slope(const Chain& far) const -> std::optional<double> {
    const std::optional<double> near_iv = atm_iv();
    const std::optional<double> far_iv = far.atm_iv();
    return near_iv && far_iv ? std::optional(*far_iv - *near_iv) : std::nullopt;
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

Portfolio::Portfolio(std::string name_, const PortfolioSpec* spec)
    : name(std::move(name_)), spec_(spec), listing_(std::make_shared<const Listing>()),
      own_(listing_, name) {
    read_own();
}

auto Portfolio::underlying_symbol() const -> std::string_view {
    if (spec_ != nullptr) {
        return spec_->underlying;
    }
    return underlying_ ? std::string_view(underlying_->symbol) : std::string_view{};
}

auto Portfolio::hedge_symbol() const -> std::string_view {
    if (spec_ != nullptr) {
        return spec_->hedge != spec_->underlying ? spec_->hedge : std::string_view{};
    }
    return hedge_instrument_ ? std::string_view(hedge_instrument_->symbol) : std::string_view{};
}

void Portfolio::set_underlying(const Contract& contract) { underlying_ = contract; }

void Portfolio::set_hedge_instrument(const Contract& contract) { hedge_instrument_ = contract; }

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

auto Portfolio::list(std::shared_ptr<const Listing> listing,
                     std::optional<std::span<const std::string>> valued)
    -> std::vector<std::string> {
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
    // The new frame: own_'s buffer when the frame is not reading it and only the chains it was
    // carried with hold values in it (a backtest, day after day: zeroing those is enough), else a
    // new one.
    PortfolioSnapshot carried;
    if (storage_ != nullptr && own_written_) {
        carried = std::move(own_);
        carried.relist(listing, *own_written_);
    } else {
        carried = PortfolioSnapshot(listing, name);
    }
    std::vector<std::uint32_t> written; // the chains whose values are carried
    const Frame& was = frame_;
    copy_time_and_underlying_quotes(was, carried);
    carried.priced = was.priced;
    carried.greeks_spot = was.greeks_spot;
    // Only the chains the frame holds values of are copied: the new frame is zeros already.
    std::vector<bool> holds(before.size(), !valued);
    for (const std::string& symbol : valued.value_or(std::span<const std::string>{})) {
        if (const std::optional<std::uint32_t> b = listing_->chain(symbol)) {
            holds[*b] = true;
        }
    }
    for (std::size_t c = 0; c < chain_from.size(); ++c) {
        if (chain_from[c] && holds[*chain_from[c]]) {
            written.push_back(static_cast<std::uint32_t>(c));
        }
    }
    for (const auto column : PortfolioSnapshot::kOptionColumns) {
        const std::span<double> to = carried.values(column);
        const std::span<const double> values = was.*column;
        for (const std::uint32_t c : written) {
            const Listing::Chain& now = listing->chains()[c];
            for (std::uint32_t i = now.first; i < now.first + now.count; ++i) {
                if (from[i]) {
                    to[i] = value_at(values, *from[i]);
                }
            }
        }
    }
    const std::span<double> forward = carried.values(&Frame::forward);
    const std::span<double> smile = carried.values(&Frame::smile);
    for (const std::uint32_t c : written) {
        forward[c] = value_at(was.forward, *chain_from[c]);
        for (std::size_t v = 0; v < kSmileValues; ++v) {
            smile[(c * kSmileValues) + v] =
                value_at(was.smile, (*chain_from[c] * kSmileValues) + v);
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
    own_written_ = std::move(written);
    read_own();
    return dropped;
}

auto Portfolio::snapshot() const -> PortfolioSnapshot { return {listing_, name}; }

void Portfolio::apply(PortfolioSnapshot& snapshot) {
    if (snapshot.listing != listing_.get()) {
        apply(static_cast<const Frame&>(snapshot));
        return;
    }
    assert(snapshot.priced);
    std::swap(own_, snapshot);
    own_written_.reset();
    read_own();
    move_time(frame_.datetime);
}

void Portfolio::apply(const Frame& frame, std::shared_ptr<const void> storage) {
    if (frame.listing != listing_.get()) {
        if (storage_ != nullptr) {
            own_.assign(frame_);
            own_written_.reset();
            read_own();
        }
        copy_time_and_underlying_quotes(frame, own_);
        read_own();
        move_time(frame.datetime);
        return;
    }
    assert(frame.priced);
    if (storage != nullptr) {
        frame_ = frame;
        storage_ = std::move(storage);
    } else {
        own_.assign(frame);
        own_written_.reset();
        read_own();
    }
    move_time(frame.datetime);
}

auto Portfolio::underlying() const -> std::optional<Underlying> {
    if (!underlying_) {
        return std::nullopt;
    }
    return Underlying(*this, false);
}

auto Portfolio::hedge() const -> std::optional<Underlying> {
    if (hedge_symbol().empty()) {
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

void Portfolio::restore_day(std::chrono::year_month_day day, double open, double high,
                            double low) {
    if (DateTime{std::chrono::sys_days{day}} != day_of_open_ || open <= 0.0 || saw_open()) {
        return;
    }
    const bool seen = day_first_minute_ >= 1;
    day_open_ = open;
    day_high_ = seen ? std::max(day_high_, high) : high;
    day_low_ = seen ? std::min(day_low_, low) : low;
    day_first_minute_ = 1;
}

void Portfolio::move_time(DateTime time) {
    if (time == DateTime{}) {
        return;
    }
    datetime_ = time;
    const SessionCalendar& calendar = this->calendar();
    const std::chrono::year_month_day date = calendar.date_of(time);
    dte_ref_ = std::chrono::sys_days{date};
    if (dte_ref_ != day_of_open_) {
        day_of_open_ = dte_ref_;
        day_open_ = day_high_ = day_low_ = 0.0;
        day_first_minute_ = -1;
    }
    const double mark = frame_.underlying_mark;
    const int minute = calendar.minutes_since_open(time);
    if (mark <= 0.0 || minute < 1 ||
        minute > (calendar.session_close(date) - calendar.open).count()) {
        return;
    }
    if (day_first_minute_ < 0) {
        day_open_ = day_high_ = day_low_ = mark;
        day_first_minute_ = minute;
    }
    day_high_ = std::max(day_high_, mark);
    day_low_ = std::min(day_low_, mark);
}

// ---- Legs

auto leg_chain(const Portfolio& portfolio, const LegRule& rule) -> std::string {
    using enum LegRule::ExpiryBy;
    if (rule.expiry_by == kNearest) {
        std::string best;
        int best_gap = std::numeric_limits<int>::max();
        for (const std::string& symbol : portfolio.chains_by_expiry(rule.min_dte, rule.max_dte)) {
            const std::optional<Chain> chain = portfolio.chain(symbol);
            const int gap = chain ? std::abs(chain->days_to_expiry() - rule.dte) : best_gap;
            if (gap < best_gap) {
                best_gap = gap;
                best = symbol;
            }
        }
        return best;
    }
    constexpr int kNoExpiryLimit = 3650;
    const std::vector<std::string> found = portfolio.chains_by_expiry(
        rule.dte, rule.expiry_by == kExactly ? rule.dte : kNoExpiryLimit);
    return found.empty() ? std::string() : found.front();
}

auto leg_option(const Chain& chain, const LegRule& rule) -> std::optional<Option> {
    const double forward = chain.forward();
    const std::span<const double> strikes = chain.strikes();
    if (forward <= 0.0 || strikes.empty()) {
        return std::nullopt;
    }
    using enum LegRule::StrikeBy;
    if (rule.strike_by == kDelta) {
        std::optional<Option> best;
        double best_gap = std::numeric_limits<double>::infinity();
        for (const double k : strikes) {
            const std::optional<Option> option = chain.option(rule.right, k);
            if (!option || !(option->iv() > 0.0) || option->multiplier() == 0.0) {
                continue;
            }
            const double gap = std::abs(option->delta() / option->multiplier() - rule.strike);
            if (gap < best_gap) {
                best_gap = gap;
                best = option;
            }
        }
        return best;
    }
    double target = forward * (1.0 + rule.strike / 100.0);
    if (rule.strike_by == kImpliedMoves) {
        const double atm = chain.atm_strike();
        const std::optional<Option> call = chain.call(atm);
        const std::optional<Option> put = chain.put(atm);
        if (!call || !put || call->bid() <= 0.0 || call->ask() <= 0.0 || put->bid() <= 0.0 ||
            put->ask() <= 0.0) {
            return std::nullopt;
        }
        target = forward * (1.0 + rule.strike * (call->mid() + put->mid()) / forward);
    }
    const auto nearest =
        std::ranges::min_element(strikes, {}, [target](double k) { return std::abs(k - target); });
    return chain.option(rule.right, *nearest);
}

} // namespace model
