#include "core/market_data/market_data_engine.hpp"
#include "model/portfolio.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "utilities/latency.hpp"
#include "utilities/log_level.hpp"
#include "utilities/text.hpp"
#include <algorithm>
#include <cstdlib>
#include <format>
#include <span>
#include <stdexcept>
#include <utility>

namespace core {

namespace {

/** Sort and drop repeats. */
template <typename T> void make_set(std::vector<T>& values) {
    std::ranges::sort(values);
    values.erase(std::ranges::unique(values).begin(), values.end());
}

/** The names' places in index, and the names not found. */
template <typename Index>
auto split_found(const Index& index, std::span<const std::string> names)
    -> std::pair<std::vector<typename Index::mapped_type>, std::vector<std::string>> {
    std::pair<std::vector<typename Index::mapped_type>, std::vector<std::string>> out;
    for (const std::string& name : names) {
        if (const auto it = index.find(name); it != index.end()) {
            out.first.push_back(it->second);
        } else {
            out.second.push_back(name);
        }
    }
    return out;
}

} // namespace

MarketDataEngine::MarketDataEngine(utilities::Logger* logger,
                                   const model::Portfolios& structure,
                                   std::unique_ptr<MarketData> market_data,
                                   SnapshotSink on_snapshot, std::chrono::milliseconds interval)
    : log_(logger, "MarketData"), structure_(structure), on_snapshot_(std::move(on_snapshot)),
      interval_(interval), market_data_(std::move(market_data)) {
    lay_out();
}

MarketDataEngine::~MarketDataEngine() { close(); }

void MarketDataEngine::start() {
    std::scoped_lock lock(control_mutex_);
    if (running_) {
        return;
    }
    if (!market_data_) {
        throw std::runtime_error("No market data source in this build: build the live engine with"
                                 " the IB gateway (./build.sh r 0 g)");
    }
    market_data_->start(*this);
    running_ = true;
    log_.write_log("Market data started", utilities::INFO);
}

void MarketDataEngine::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!running_) {
        return;
    }
    market_data_->stop();
    running_ = false;
    reset(); // the source's thread is gone: the next stream starts from nothing
    log_.write_log("Market data stopped", utilities::INFO);
}

auto MarketDataEngine::ask_close(const std::string& underlying, std::chrono::year_month_day day)
    -> bool {
    std::unique_lock lock(control_mutex_, std::try_to_lock); // not while starting or stopping
    if (!lock.owns_lock() || !running_) {
        return false;
    }
    market_data_->ask_close(underlying, day);
    return true;
}

void MarketDataEngine::closed(const std::string& underlying, std::chrono::year_month_day day,
                              double close) {
    if (on_close_) {
        on_close_(underlying, day, close);
    }
}

void MarketDataEngine::load_contracts() {
    std::scoped_lock lock(control_mutex_);
    if (running_) {
        throw std::logic_error("Stop the market data before loading its contracts again");
    }
    lay_out();
    log_.write_log("Market data loaded the day's contracts", utilities::INFO);
}

void MarketDataEngine::lay_out() {
    books_.clear();
    chains_.clear();
    chain_index_.clear();
    option_slots_.clear();
    std::vector<std::string> names = structure_.portfolio_names();
    std::ranges::sort(names);
    for (const std::string& name : names) {
        const model::Portfolio* portfolio = structure_.find_portfolio(name);
        const auto index = static_cast<std::uint32_t>(books_.size());
        Book& book = books_.emplace_back();
        if (const std::optional<model::Underlying> u = portfolio->underlying()) {
            book.underlying = portfolio->contract(u->symbol());
        }
        if (!portfolio->hedge_symbol.empty()) {
            book.hedge = portfolio->contract(portfolio->hedge_symbol);
        }
        book.portfolio = portfolio;
        book.listing = portfolio->shared_listing();
        book.snapshot = portfolio->snapshot();
        const model::Listing& listing = *book.listing;
        book.option_quotes.resize(listing.size());
        book.option_changed.assign(listing.size(), 0);
        for (std::uint32_t column = 0; column < listing.size(); ++column) {
            option_slots_[listing.contract(column).symbol] = {
                .portfolio = index, .option = static_cast<std::int32_t>(column)};
        }
        for (std::uint32_t c = 0; c < listing.chains().size(); ++c) {
            chains_.push_back({.portfolio = index, .index = c});
            chain_index_.emplace(listing.chains()[c].symbol, chains_.size() - 1);
        }
    }
    next_replan_ = {};

    // What was subscribed and held, found again among the new contracts (by name).
    std::scoped_lock lock(subscriptions_mutex_);
    subscriptions_.clear();
    for (const auto& [strategy_name, chains] : subscribed_) {
        subscriptions_[strategy_name] = resolve_chains(chains).first;
    }
    streamed_ = resolve_options(streamed_options_).first;
}

auto MarketDataEngine::resolve_chains(std::span<const std::string> chains) const
    -> std::pair<std::vector<std::size_t>, std::vector<std::string>> {
    return split_found(chain_index_, chains);
}

auto MarketDataEngine::resolve_options(std::span<const std::string> options) const
    -> std::pair<std::vector<QuoteSlot>, std::vector<std::string>> {
    return split_found(option_slots_, options);
}

void MarketDataEngine::subscribe_chains(const std::string& strategy_name,
                                        std::span<const std::string> chains) {
    auto [found, unknown] = resolve_chains(chains);
    {
        std::scoped_lock lock(subscriptions_mutex_);
        subscribed_[strategy_name] = {chains.begin(), chains.end()};
        subscriptions_[strategy_name] = std::move(found);
    }
    if (market_data_) { // nothing streams without a source
        log_.write_log(std::format("{} subscribes {}", strategy_name, utilities::join(chains)),
                       utilities::INFO);
        if (!unknown.empty()) {
            log_.write_log(std::format("{}: no chain {} to stream", strategy_name,
                                       utilities::join(unknown)),
                           utilities::WARNING);
        }
    }
    subscriptions_changed();
}

void MarketDataEngine::unsubscribe_chains(const std::string& strategy_name) {
    {
        std::scoped_lock lock(subscriptions_mutex_);
        subscribed_.erase(strategy_name);
        subscriptions_.erase(strategy_name);
    }
    subscriptions_changed();
}

void MarketDataEngine::set_held_options(std::span<const std::string> options) {
    {
        std::scoped_lock lock(subscriptions_mutex_);
        held_options_ = {options.begin(), options.end()};
    }
    subscriptions_changed();
}

void MarketDataEngine::add_ordered_options(const model::OrderRequest& request) {
    bool added = false;
    {
        std::scoped_lock lock(subscriptions_mutex_);
        const auto add = [&](const std::string& symbol) {
            if (model::parse_option_symbol(symbol) && ordered_options_.insert(symbol).second) {
                added = true;
            }
        };
        for (const model::Leg& leg : request.legs) {
            add(leg.symbol);
        }
    }
    if (added) {
        subscriptions_changed();
    }
}

void MarketDataEngine::clear_ordered_options() {
    std::scoped_lock lock(subscriptions_mutex_);
    ordered_options_.clear();
}

auto MarketDataEngine::subscribed_chains() const
    -> std::unordered_map<std::string, std::vector<std::string>> {
    std::scoped_lock lock(subscriptions_mutex_);
    return subscribed_;
}

auto MarketDataEngine::held_and_ordered_options() const -> std::vector<std::string> {
    std::scoped_lock lock(subscriptions_mutex_);
    return held_and_ordered_options_locked();
}

auto MarketDataEngine::chains_wanted() const -> std::vector<std::string> {
    std::scoped_lock lock(subscriptions_mutex_);
    std::vector<std::string> out;
    for (const auto& [_, chains] : subscribed_) {
        out.insert(out.end(), chains.begin(), chains.end());
    }
    for (const std::string& option : held_and_ordered_options_locked()) {
        if (const std::optional<model::OptionId> id = model::parse_option_symbol(option)) {
            out.push_back(model::chain_symbol(id->root, id->expiry));
        }
    }
    make_set(out);
    return out;
}

auto MarketDataEngine::held_and_ordered_options_locked() const -> std::vector<std::string> {
    std::vector<std::string> out = held_options_;
    out.insert(out.end(), ordered_options_.begin(), ordered_options_.end());
    make_set(out);
    return out;
}

void MarketDataEngine::subscriptions_changed() {
    wanted_changes_.fetch_add(1, std::memory_order_acq_rel);
    std::vector<std::string> unknown;
    {
        std::scoped_lock lock(subscriptions_mutex_);
        std::vector<std::string> options = held_and_ordered_options_locked();
        if (options != streamed_options_) { // the same options: the streams stay as they are
            auto [slots, not_found] = resolve_options(options);
            streamed_options_ = std::move(options);
            streamed_ = std::move(slots);
            unknown = std::move(not_found);
        }
    }
    // An option without a contract here has expired (its contract is gone) or was never synced.
    if (market_data_) {
        for (const std::string& symbol : unknown) {
            log_.write_log("Held option not streamed (no contract): " + symbol, utilities::DEBUG);
        }
    }
}

auto MarketDataEngine::instruments(std::size_t max_lines) -> std::vector<Instrument> {
    std::vector<std::size_t> wanted;
    std::vector<QuoteSlot> held;
    {
        std::scoped_lock lock(subscriptions_mutex_);
        for (const auto& [_, chains] : subscriptions_) {
            wanted.insert(wanted.end(), chains.begin(), chains.end());
        }
        held = streamed_;
    }
    make_set(wanted);

    // The underlyings (a window centers on their price) and the hedge instruments first.
    std::vector<std::uint32_t> portfolios;
    portfolios.reserve(wanted.size() + held.size());
    for (const std::size_t chain : wanted) {
        portfolios.push_back(chains_[chain].portfolio);
    }
    for (const QuoteSlot& slot : held) {
        portfolios.push_back(slot.portfolio);
    }
    make_set(portfolios);
    std::vector<Instrument> out;
    std::vector<const model::Contract*> lines; // the contracts so far, each once
    for (const QuoteSlot& slot : held) {
        const model::Contract* c = &books_[slot.portfolio].listing->contract(slot.option);
        out.push_back({c, slot});
        lines.push_back(c);
    }
    for (const std::uint32_t p : portfolios) {
        const Book& book = books_[p];
        if (book.underlying != nullptr) {
            out.push_back({book.underlying, {.portfolio = p, .option = QuoteSlot::kUnderlying}});
            lines.push_back(book.underlying);
        }
        if (book.hedge != nullptr) {
            out.push_back({book.hedge, {.portfolio = p, .option = QuoteSlot::kHedge}});
            lines.push_back(book.hedge);
        }
    }
    make_set(lines);

    std::size_t strikes = 0; // all
    if (max_lines > 0 && !wanted.empty()) {
        const std::size_t left = max_lines > lines.size() ? max_lines - lines.size() : 0;
        strikes = std::max<std::size_t>(1, left / wanted.size() / 2); // a call and a put each
    }
    for (const std::size_t chain : wanted) {
        window(chains_[chain], strikes, out);
    }
    // A held option inside a window is streamed once.
    std::vector<QuoteSlot> seen;
    std::erase_if(out, [&seen](const Instrument& i) {
        if (std::ranges::contains(seen, i.slot)) {
            return true;
        }
        seen.push_back(i.slot);
        return false;
    });
    return out;
}

void MarketDataEngine::window(Chain& chain, std::size_t strikes, std::vector<Instrument>& out) {
    const Book& book = books_[chain.portfolio];
    const model::Listing::Chain& listed = book.listing->chains()[chain.index];
    const auto n = static_cast<std::ptrdiff_t>(listed.strikes.size());
    std::ptrdiff_t first = 0;
    std::ptrdiff_t count = n;
    if (strikes > 0 && std::cmp_less(strikes, n)) {
        const Quote& u = book.underlying_quote;
        const double price = model::mark(u.bid, u.ask, u.last);
        if (price <= 0.0) {
            return;
        }
        // The strike nearest the price.
        const std::vector<double>& k = listed.strikes;
        std::ptrdiff_t atm = std::ranges::lower_bound(k, price) - k.begin();
        if (atm == n || (atm > 0 && price - k[atm - 1] < k[atm] - price)) {
            --atm;
        }
        count = static_cast<std::ptrdiff_t>(strikes);
        const std::ptrdiff_t slack = std::max<std::ptrdiff_t>(1, count / 4);
        if (chain.center < 0 || std::abs(atm - chain.center) > slack) {
            chain.center = atm;
        }
        first = std::clamp<std::ptrdiff_t>(chain.center - count / 2, 0, n - count);
    }
    for (std::ptrdiff_t i = first; i < first + count; ++i) {
        for (const std::int32_t column : {listed.calls[i], listed.puts[i]}) {
            if (column >= 0) {
                out.push_back({&book.listing->contract(column), {chain.portfolio, column}});
            }
        }
    }
}

auto MarketDataEngine::at(QuoteSlot slot) -> Quote& {
    Book& book = books_[slot.portfolio];
    switch (slot.option) {
    case QuoteSlot::kUnderlying:
        return book.underlying_quote;
    case QuoteSlot::kHedge:
        return book.hedge_quote;
    default:
        return book.option_quotes[slot.option];
    }
}

auto MarketDataEngine::Quote::operator[](QuoteField field) -> double& {
    switch (field) {
    case QuoteField::Bid:
        return bid;
    case QuoteField::Ask:
        return ask;
    case QuoteField::Last:
        return last;
    case QuoteField::BidSize:
        return bid_size;
    case QuoteField::AskSize:
        return ask_size;
    }
    std::unreachable();
}

void MarketDataEngine::quote(QuoteSlot slot, QuoteField field, double value,
                             model::DateTime time) {
    Book& book = books_[slot.portfolio];
    book.quoted_at = std::max(book.quoted_at, time);
    double& quoted = at(slot)[field];
    if (field == QuoteField::BidSize || field == QuoteField::AskSize) {
        quoted = value;
        return;
    }
    if (quoted != value) {
        quoted = value;
        mark_changed(slot);
    }
}

void MarketDataEngine::clear(QuoteSlot slot) {
    at(slot) = {};
    mark_changed(slot);
}

void MarketDataEngine::mark_changed(QuoteSlot slot) {
    Book& book = books_[slot.portfolio];
    if (!book.changed) {
        book.changed_tsc = utilities::tsc_now(); // where the snapshot's latency starts
    }
    book.changed = true;
    if (slot.option >= 0) {
        book.option_changed[static_cast<std::size_t>(slot.option)] = 1;
    } else if (slot.option == QuoteSlot::kUnderlying) {
        book.underlying_changed = true;
    }
}

auto MarketDataEngine::replan_due(std::chrono::steady_clock::time_point now) -> bool {
    if (now < next_replan_) {
        return false;
    }
    next_replan_ = now + interval_;
    return true;
}

void MarketDataEngine::publish() {
    for (Book& book : books_) {
        if (!book.changed) {
            continue;
        }
        book.changed = false;
        model::PortfolioSnapshot& s = book.snapshot;
        s.datetime = book.quoted_at;
        s.quoted_tsc = book.changed_tsc;
        const Quote& u = book.underlying_quote;
        s.underlying_bid = u.bid;
        s.underlying_ask = u.ask;
        s.underlying_mark = model::mark(u.bid, u.ask, u.last);
        const Quote& h = book.hedge_quote;
        s.hedge_bid = h.bid;
        s.hedge_ask = h.ask;
        s.hedge_mark = model::mark(h.bid, h.ask, h.last);
        const std::span<double> bid = s.values(&model::Frame::bid);
        const std::span<double> ask = s.values(&model::Frame::ask);
        const std::span<double> bid_size = s.values(&model::Frame::bid_size);
        const std::span<double> ask_size = s.values(&model::Frame::ask_size);
        for (std::size_t i = 0; i < book.option_quotes.size(); ++i) {
            const Quote& q = book.option_quotes[i];
            bid[i] = q.bid;
            ask[i] = q.ask;
            bid_size[i] = q.bid_size;
            ask_size[i] = q.ask_size;
        }
        // All priced on the first snapshot, or one after reset(). Live: an expiry's time moves in
        // steps of kLiveTimeStep (see price_snapshot).
        book.portfolio->price_snapshot(s, book.option_changed, book.underlying_changed,
                                       model::Portfolio::kLiveTimeStep);
        std::ranges::fill(book.option_changed, 0);
        book.underlying_changed = false;
        if (on_snapshot_) {
            on_snapshot_(s);
        }
    }
}

void MarketDataEngine::reset() {
    for (Book& book : books_) {
        book.underlying_quote = {};
        book.hedge_quote = {};
        std::ranges::fill(book.option_quotes, Quote{});
        book.quoted_at = {};
        book.changed = false;
        std::ranges::fill(book.option_changed, 0);
        book.underlying_changed = false;
        book.snapshot.priced = false; // the next is priced whole
    }
    for (Chain& chain : chains_) {
        chain.center = -1;
    }
    next_replan_ = {};
}

} // namespace core
