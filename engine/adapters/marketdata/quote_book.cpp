/** QuoteBook: subscriptions → instruments to stream; quotes → snapshots. */

#include "quote_book.hpp"
#include "../../model/portfolio.hpp"
#include <algorithm>
#include <cstdlib>
#include <utility>

namespace adapters {

namespace {

/** Sort and drop repeats. */
template <typename T> void make_set(std::vector<T>& values) {
    std::ranges::sort(values);
    values.erase(std::ranges::unique(values).begin(), values.end());
}

} // namespace

QuoteBook::QuoteBook(const model::PortfolioStructure& structure, SnapshotSink on_snapshot,
                     std::chrono::milliseconds interval)
    : on_snapshot_(std::move(on_snapshot)), interval_(interval) {
    std::vector<std::string> names = structure.get_all_portfolio_names();
    std::ranges::sort(names);
    for (const std::string& name : names) {
        const model::PortfolioData* portfolio = structure.get_portfolio(name);
        const auto index = static_cast<std::uint32_t>(books_.size());
        Book& book = books_.emplace_back();
        if (portfolio->underlying) {
            book.underlying = structure.get_contract(portfolio->underlying->symbol);
        }
        if (portfolio->hedge_instrument) {
            book.hedge = structure.get_contract(portfolio->hedge_instrument->symbol);
        }
        const std::vector<model::OptionData*>& order = portfolio->option_apply_order();
        std::unordered_map<const model::OptionData*, std::int32_t> column;
        for (const model::OptionData* option : order) {
            column[option] = static_cast<std::int32_t>(book.options.size());
            if (option != nullptr) {
                option_slots_[option->symbol] = {.portfolio = index, .option = column[option]};
            }
            book.options.push_back(option != nullptr ? structure.get_contract(option->symbol)
                                                     : nullptr);
        }
        book.option_quotes.resize(order.size());
        book.option_changed.assign(order.size(), 0);
        book.portfolio = portfolio;
        book.snapshot.portfolio_name = name;
        book.snapshot.bid.resize(order.size());
        book.snapshot.ask.resize(order.size());
        book.snapshot.mark.resize(order.size());

        for (const auto& [symbol, chain_data] : portfolio->chains) {
            if (!chain_data) {
                continue;
            }
            Chain& chain = chains_.emplace_back();
            chain.portfolio = index;
            const auto column_of = [&column](const auto& options, double strike) {
                const auto it = options.find(strike);
                return it != options.end() ? column.at(it->second) : -1;
            };
            for (const double strike : chain_data->strikes) { // ascending
                const std::int32_t call = column_of(chain_data->calls, strike);
                const std::int32_t put = column_of(chain_data->puts, strike);
                if (call < 0 && put < 0) {
                    continue;
                }
                chain.strikes.push_back(strike);
                chain.calls.push_back(call);
                chain.puts.push_back(put);
            }
            chain_index_.emplace(symbol, chains_.size() - 1);
        }
    }
}

auto QuoteBook::subscribe(const std::string& strategy_name, std::span<const std::string> chains)
    -> std::vector<std::string> {
    std::vector<std::size_t> found;
    std::vector<std::string> unknown;
    for (const std::string& symbol : chains) {
        if (const auto it = chain_index_.find(symbol); it != chain_index_.end()) {
            found.push_back(it->second);
        } else {
            unknown.push_back(symbol);
        }
    }
    std::scoped_lock lock(subscriptions_mutex_);
    subscriptions_[strategy_name] = std::move(found);
    return unknown;
}

void QuoteBook::unsubscribe(const std::string& strategy_name) {
    std::scoped_lock lock(subscriptions_mutex_);
    subscriptions_.erase(strategy_name);
}

auto QuoteBook::hold(std::span<const std::string> options) -> std::vector<std::string> {
    std::vector<QuoteSlot> slots;
    std::vector<std::string> unknown;
    for (const std::string& symbol : options) {
        if (const auto it = option_slots_.find(symbol); it != option_slots_.end()) {
            slots.push_back(it->second);
        } else {
            unknown.push_back(symbol);
        }
    }
    std::scoped_lock lock(subscriptions_mutex_);
    held_ = std::move(slots);
    return unknown;
}

auto QuoteBook::instruments(std::size_t max_lines) -> std::vector<Instrument> {
    std::vector<std::size_t> wanted;
    std::vector<QuoteSlot> held;
    {
        std::scoped_lock lock(subscriptions_mutex_);
        for (const auto& [_, chains] : subscriptions_) {
            wanted.insert(wanted.end(), chains.begin(), chains.end());
        }
        held = held_;
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
    std::vector<const model::ContractData*> lines; // the contracts so far, each once
    for (const QuoteSlot& slot : held) {
        if (const model::ContractData* c = books_[slot.portfolio].options[slot.option]) {
            out.push_back({c, slot});
            lines.push_back(c);
        }
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
    std::vector<std::pair<std::uint32_t, std::int32_t>> seen;
    std::erase_if(out, [&seen](const Instrument& i) {
        const std::pair key{i.slot.portfolio, i.slot.option};
        if (std::ranges::find(seen, key) != seen.end()) {
            return true;
        }
        seen.push_back(key);
        return false;
    });
    return out;
}

void QuoteBook::window(Chain& chain, std::size_t strikes, std::vector<Instrument>& out) {
    const auto n = static_cast<std::ptrdiff_t>(chain.strikes.size());
    std::ptrdiff_t first = 0;
    std::ptrdiff_t count = n;
    if (strikes > 0 && std::cmp_less(strikes, n)) {
        const Quote& u = books_[chain.portfolio].underlying_quote;
        const double price = model::mark(u.bid, u.ask, u.last);
        if (price <= 0.0) {
            return;
        }
        // The strike nearest the price.
        std::ptrdiff_t atm = std::ranges::lower_bound(chain.strikes, price) - chain.strikes.begin();
        if (atm == n || (atm > 0 && price - chain.strikes[atm - 1] < chain.strikes[atm] - price)) {
            --atm;
        }
        count = static_cast<std::ptrdiff_t>(strikes);
        const std::ptrdiff_t slack = std::max<std::ptrdiff_t>(1, count / 4);
        if (chain.center < 0 || std::abs(atm - chain.center) > slack) {
            chain.center = atm;
        }
        first = std::clamp<std::ptrdiff_t>(chain.center - count / 2, 0, n - count);
    }
    const Book& book = books_[chain.portfolio];
    for (std::ptrdiff_t i = first; i < first + count; ++i) {
        for (const std::int32_t column : {chain.calls[i], chain.puts[i]}) {
            if (column >= 0 && book.options[column] != nullptr) {
                out.push_back({book.options[column], {chain.portfolio, column}});
            }
        }
    }
}

auto QuoteBook::at(QuoteSlot slot) -> Quote& {
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

auto QuoteBook::Quote::operator[](QuoteField field) -> double& {
    switch (field) {
    case QuoteField::Bid:
        return bid;
    case QuoteField::Ask:
        return ask;
    case QuoteField::Last:
        return last;
    }
    std::unreachable();
}

void QuoteBook::quote(QuoteSlot slot, QuoteField field, double price, model::DateTime time) {
    Book& book = books_[slot.portfolio];
    book.quoted_at = std::max(book.quoted_at, time);
    double& value = at(slot)[field];
    if (value != price) {
        value = price;
        mark_changed(slot);
    }
}

void QuoteBook::clear(QuoteSlot slot) {
    at(slot) = {};
    mark_changed(slot);
}

void QuoteBook::mark_changed(QuoteSlot slot) {
    Book& book = books_[slot.portfolio];
    book.changed = true;
    if (slot.option >= 0) {
        book.option_changed[static_cast<std::size_t>(slot.option)] = 1;
    } else if (slot.option == QuoteSlot::kUnderlying) {
        book.underlying_changed = true;
    }
}

auto QuoteBook::replan_due(std::chrono::steady_clock::time_point now) -> bool {
    if (now < next_replan_) {
        return false;
    }
    next_replan_ = now + interval_;
    return true;
}

void QuoteBook::publish() {
    for (Book& book : books_) {
        if (!book.changed) {
            continue;
        }
        book.changed = false;
        model::PortfolioSnapshot& s = book.snapshot;
        s.datetime = book.quoted_at;
        const Quote& u = book.underlying_quote;
        s.underlying_bid = u.bid;
        s.underlying_ask = u.ask;
        s.underlying_mark = model::mark(u.bid, u.ask, u.last);
        const Quote& h = book.hedge_quote;
        s.hedge_bid = h.bid;
        s.hedge_ask = h.ask;
        s.hedge_mark = model::mark(h.bid, h.ask, h.last);
        for (std::size_t i = 0; i < book.option_quotes.size(); ++i) {
            const Quote& q = book.option_quotes[i];
            s.bid[i] = q.bid;
            s.ask[i] = q.ask;
            s.mark[i] = model::mark(q.bid, q.ask);
        }
        if (book.portfolio != nullptr) { // the first snapshot, or one after reset(): all
            book.portfolio->price_snapshot(s, book.option_changed, book.underlying_changed);
        }
        std::ranges::fill(book.option_changed, 0);
        book.underlying_changed = false;
        if (on_snapshot_) {
            on_snapshot_(s);
        }
    }
}

void QuoteBook::reset() {
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

} // namespace adapters
