#include "core/market_data/market_data_engine.hpp"
#include "model/portfolio.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "pricing/pricing.hpp"
#include "utilities/latency.hpp"
#include "utilities/log_level.hpp"
#include "utilities/text.hpp"
#include "utilities/thread_pool.hpp"
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

} // namespace

MarketDataEngine::MarketDataEngine(utilities::Logger* logger,
                                   const model::Portfolios& structure,
                                   const StrategyEngine& strategies,
                                   std::unique_ptr<MarketData> market_data,
                                   SnapshotSink on_snapshot, std::chrono::milliseconds interval)
    : log_(logger, "MarketData"), structure_(structure), strategies_(strategies),
      on_snapshot_(std::move(on_snapshot)), interval_(interval),
      market_data_(std::move(market_data)) {
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

void MarketDataEngine::use_rates(model::RateCurve rates) {
    rates_.store(std::make_shared<const model::RateCurve>(std::move(rates)));
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

auto MarketDataEngine::ask_day_range(const std::string& underlying,
                                     std::chrono::year_month_day day) -> bool {
    std::unique_lock lock(control_mutex_, std::try_to_lock); // not while starting or stopping
    if (!lock.owns_lock() || !running_) {
        return false;
    }
    market_data_->ask_day_range(underlying, day);
    return true;
}

void MarketDataEngine::day_range(const std::string& underlying, std::chrono::year_month_day day,
                                 double open, double high, double low) {
    if (on_day_range_) {
        on_day_range_(underlying, day, open, high, low);
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
        if (const std::string_view hedge = portfolio->hedge_symbol(); !hedge.empty()) {
            book.hedge = portfolio->contract(hedge);
        }
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
    planned_.reset();
    warned_.clear();
}

auto MarketDataEngine::instruments(std::size_t max_lines) -> std::vector<Instrument> {
    const std::shared_ptr<const StrategyEngine::Wanted> asked = strategies_.wanted();
    // Found by name in the day's contracts: an option held may have expired, or not be synced.
    std::vector<std::size_t> wanted;
    for (const std::string& symbol : asked->subscribed) {
        if (const auto it = chain_index_.find(symbol); it != chain_index_.end()) {
            wanted.push_back(it->second);
        } else if (warned_.insert(symbol).second) {
            log_.write_log("No chain " + symbol + " to stream: not among the day's contracts",
                           utilities::WARNING);
        }
    }
    std::vector<QuoteSlot> held;
    for (const std::string& symbol : asked->options) {
        if (const auto it = option_slots_.find(symbol); it != option_slots_.end()) {
            held.push_back(it->second);
        } else if (warned_.insert(symbol).second) {
            log_.write_log("Held option not streamed (no contract): " + symbol, utilities::DEBUG);
        }
    }
    if (planned_ == nullptr || planned_->subscribed != asked->subscribed ||
        planned_->options != asked->options) {
        log_.write_log(std::format("Streaming chains {}; {} options held or ordered",
                                   asked->subscribed.empty() ? std::string("none")
                                                             : utilities::join(asked->subscribed),
                                   asked->options.size()),
                       utilities::INFO);
    }
    planned_ = asked;
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
        // steps of kLiveTimeStep (see price_snapshot), a chain's smile is fitted once a minute.
        pricing::price_snapshot(s, *rates_.load(), iv_price_mode_, book.option_changed,
                                book.underlying_changed, pricing::kLiveTimeStep,
                                &utilities::shared_pool());
        pricing::fit_smiles(s, pricing::kLiveFitStep, &utilities::shared_pool());
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
