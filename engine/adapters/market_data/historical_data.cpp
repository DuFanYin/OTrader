#include "adapters/market_data/historical_data.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "pricing/pricing.hpp"
#include "utilities/thread_pool.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adapters {

/** A chain of the listing (by its index), loaded once. */
struct HistoricalData::Chain {
    std::once_flag once;
    std::atomic<bool> loaded{false};
    std::optional<StoredChain> stored; // its minutes; none when it could not be read
    std::string error;                 // why not; both set before loaded
};

HistoricalData::HistoricalData(utilities::Logger* logger) : log_(logger, "HistoricalData") {
    index_.listing = std::make_shared<const model::Listing>();
}

HistoricalData::~HistoricalData() = default;

void HistoricalData::set_risk_free_rate(double rate) {
    if (std::isfinite(rate)) {
        risk_free_rate_ = rate;
    }
}

void HistoricalData::set_iv_price_mode(std::string_view mode) {
    if (const std::string_view known = model::iv_price_mode_of(mode); !known.empty()) {
        iv_price_mode_ = known;
    }
}

void HistoricalData::load(const std::string& parquet_path, const std::string& cache_dir) {
    loaded_ = false;
    error_.clear();
    path_ = parquet_path;
    cache_dir_ = cache_dir;
    try {
        const std::string stored =
            cache_dir.empty() ? std::string{} : stored_day(cache_dir, parquet_path);
        std::optional<DayIndex> index = stored.empty() ? std::nullopt : read_stored_day(stored);
        if (!index) {
            auto read = read_day_index(parquet_path);
            if (!read) {
                error_ = std::move(read.error());
                log_.write_log("Failed to load " + parquet_path + ": " + error_, utilities::ERROR);
                return;
            }
            index = std::move(*read);
            if (!stored.empty()) {
                store_day(stored, *index);
            }
        }
        index_ = std::move(*index);
        rates_ = index_.rates.value_or(model::RateCurve::flat(risk_free_rate_));
        chains_.clear();
        for (std::size_t c = 0; c < index_.listing->chains().size(); ++c) {
            chains_.push_back(std::make_unique<Chain>());
        }
        price_minutes();
        loaded_ = true;
    } catch (const std::exception& e) {
        error_ = e.what();
        log_.write_log("Failed to load " + parquet_path + ": " + error_, utilities::ERROR);
    }
}

void HistoricalData::price_minutes() {
    const std::size_t n = index_.times.size();
    const std::span<const model::DateTime> expiries = index_.listing->expiries();
    greeks_spot_.assign(n, 0.0);
    expiry_years_.assign(n * expiries.size(), 0.0);
    expiry_rate_.assign(n * expiries.size(), 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const model::DateTime time{model::DateTime::duration{index_.times[i]}};
        greeks_spot_[i] = model::mark(index_.underlying_bid[i], index_.underlying_ask[i]);
        for (std::size_t e = 0; e < expiries.size(); ++e) {
            const double t = model::years_to_expiry(time, expiries[e]);
            expiry_years_[(i * expiries.size()) + e] = t;
            expiry_rate_[(i * expiries.size()) + e] = rates_.at(t); // as pricing has it
        }
    }
}

void HistoricalData::load_chains(std::span<const std::string> chains) {
    if (!loaded_) {
        return;
    }
    for (const std::string& symbol : chains) {
        if (const std::optional<std::uint32_t> c = index_.listing->chain(symbol)) {
            std::call_once(chains_[*c]->once, [this, c] { load_chain(*c); });
        }
    }
}

void HistoricalData::load_all_chains() {
    std::vector<std::string> all;
    for (const model::Listing::Chain& listed : index_.listing->chains()) {
        all.push_back(listed.symbol);
    }
    load_chains(all);
}

auto HistoricalData::loaded_chains() const -> std::vector<std::string> {
    std::vector<std::string> out;
    for (std::size_t c = 0; c < chains_.size(); ++c) {
        if (chains_[c]->loaded.load(std::memory_order_acquire)) {
            out.push_back(index_.listing->chains()[c].symbol);
        }
    }
    return out;
}

auto HistoricalData::load_error() const -> std::string {
    std::string why = error_;
    for (std::size_t c = 0; c < chains_.size(); ++c) {
        const Chain& chain = *chains_[c];
        if (chain.loaded.load(std::memory_order_acquire) && !chain.error.empty()) {
            why += std::format("{}{}: {}", why.empty() ? "" : "; ",
                               index_.listing->chains()[c].symbol, chain.error);
        }
    }
    return why;
}

void HistoricalData::load_chain(std::size_t c) {
    Chain& chain = *chains_[c];
    const model::Listing::Chain& listed = index_.listing->chains()[c];
    const std::span<const model::Contract> options =
        index_.listing->contracts().subspan(listed.first, listed.count);
    try {
        const std::string stored =
            cache_dir_.empty() ? std::string{}
                               : stored_chain(cache_dir_, path_, listed.symbol,
                                              pricing::fingerprint(), risk_free_rate_,
                                              iv_price_mode_);
        chain.stored = stored.empty() ? std::nullopt
                                      : StoredChain::read(stored, listed.symbol, options,
                                                          index_.times.size());
        if (!chain.stored) {
            StoredChain made(listed.symbol, options, index_.times.size());
            if (const auto quoted = read_chain_quotes(path_, index_.times, options, made);
                !quoted) {
                throw std::runtime_error(quoted.error());
            }
            price_chain(c, made);
            if (!stored.empty() && store_chains_ && priced_minutes_.empty()) {
                made.store(stored);
            }
            chain.stored = std::move(made);
        }
    } catch (const std::exception& e) {
        chain.stored.reset();
        chain.error = e.what();
        log_.write_log(std::format("Chain {} of {} not loaded: {}", listed.symbol, path_,
                                   chain.error),
                       utilities::ERROR);
    }
    chain.loaded.store(true, std::memory_order_release);
}

void HistoricalData::price_chain(std::size_t c, StoredChain& stored) const {
    const model::Listing::Chain& listed = index_.listing->chains()[c];
    std::vector<model::Contract> contracts;
    contracts.reserve(listed.count);
    for (std::uint32_t k = listed.first; k < listed.first + listed.count; ++k) {
        contracts.push_back(index_.listing->contract(k));
    }
    // The chain alone: its forward and IVs are its quotes', its expiry's and the underlying's.
    const auto alone = std::make_shared<const model::Listing>(std::move(contracts));
    constexpr std::size_t kIv = 4; // the IV's column
    const auto& option_columns = model::PortfolioSnapshot::kOptionColumns;
    const std::size_t n = priced_minutes_.empty() ? index_.times.size() : priced_minutes_.size();
    utilities::shared_pool().parallel_for(
        0, n,
        [&](std::size_t m) {
            const std::size_t i = priced_minutes_.empty() ? m : priced_minutes_[m];
            if (i >= index_.times.size()) {
                return;
            }
            model::Frame f;
            f.listing = alone.get();
            f.portfolio_name = index_.portfolio;
            f.datetime = model::DateTime{model::DateTime::duration{index_.times[i]}};
            f.underlying_bid = index_.underlying_bid[i];
            f.underlying_ask = index_.underlying_ask[i];
            f.underlying_mark = model::mark(f.underlying_bid, f.underlying_ask);
            for (std::size_t q = 0; q < kIv; ++q) {
                f.*option_columns[q] = std::as_const(stored).row(q, i);
            }
            model::PortfolioSnapshot s(alone, index_.portfolio);
            s.assign(f);
            pricing::price_snapshot(s, rates_, iv_price_mode_); // on the pool already
            pricing::fit_smiles(s);
            std::ranges::copy(s.iv, stored.row(kIv, i).begin());
            stored.set_forward(i, s.forward[0]);
            std::ranges::copy(s.smile, stored.smile(i).begin());
        },
        /*inline_threshold=*/1);
}

void HistoricalData::build_portfolio(model::Portfolios& target) const {
    const std::string& name = index_.portfolio;
    target.ensure_portfolio(name); // a hedge its spec names is not in the day: none
    model::Portfolio* port = target.find_portfolio(name);
    target.replace_contracts(name, index_.underlying, index_.listing);
    if (index_.day) {
        port->set_trading_day(*index_.day);
    }
}

void HistoricalData::fill(std::size_t i, model::PortfolioSnapshot& minute,
                          const std::vector<std::uint32_t>* chains) const {
    if (minute.shared_listing() != index_.listing) {
        minute = model::PortfolioSnapshot(index_.listing, index_.portfolio);
    }
    if (!loaded_ || i >= index_.times.size()) {
        return;
    }
    minute.datetime = model::DateTime{model::DateTime::duration{index_.times[i]}};
    minute.underlying_bid = index_.underlying_bid[i];
    minute.underlying_ask = index_.underlying_ask[i];
    minute.underlying_mark = model::mark(minute.underlying_bid, minute.underlying_ask);
    const auto& option_columns = model::PortfolioSnapshot::kOptionColumns;
    const std::span<double> forward = minute.values(&model::Frame::forward);
    const std::span<double> smile = minute.values(&model::Frame::smile);
    const auto fill_chain = [&](std::size_t c) {
        const Chain& chain = *chains_[c];
        if (!chain.loaded.load(std::memory_order_acquire) || !chain.stored) {
            return;
        }
        const std::uint32_t first = index_.listing->chains()[c].first;
        for (std::size_t q = 0; q < StoredChain::kColumns; ++q) {
            std::ranges::copy(chain.stored->row(q, i),
                              minute.values(option_columns[q]).begin() + first);
        }
        forward[c] = chain.stored->forward(i);
        std::ranges::copy(chain.stored->smile(i), smile.begin() + (c * model::kSmileValues));
    };
    if (chains != nullptr) {
        std::ranges::for_each(*chains, fill_chain);
    } else {
        for (std::size_t c = 0; c < chains_.size(); ++c) {
            fill_chain(c);
        }
    }
    const std::size_t expiries = index_.listing->expiries().size();
    const auto at = static_cast<std::ptrdiff_t>(i * expiries);
    std::copy_n(expiry_years_.begin() + at, expiries,
                minute.values(&model::Frame::expiry_years).begin());
    std::copy_n(expiry_rate_.begin() + at, expiries,
                minute.values(&model::Frame::expiry_rate).begin());
    minute.priced = true;
    minute.greeks_spot = greeks_spot_[i];
    minute.iv_price_mode = iv_price_mode_;
}

LoadedDay load_day(const std::string& parquet_path, double risk_free_rate,
                   std::string_view iv_price_mode, const std::string& cache_dir,
                   bool store_chains) {
    LoadedDay day = std::make_shared<HistoricalData>();
    day->set_risk_free_rate(risk_free_rate);
    day->set_iv_price_mode(iv_price_mode);
    day->set_store_chains(store_chains);
    day->load(parquet_path, cache_dir);
    return day;
}

// ---- Replay

auto Replay::roll(const HistoricalData& day) -> std::vector<std::string> {
    const std::string& name = day.portfolio_name();
    if (structure_.find_portfolio(name) == nullptr) {
        day.build_portfolio(structure_);
        return {};
    }
    // The portfolio's frame, and the snapshot it reads, hold the values of the chains filled since
    // the last day was rolled in: only theirs are carried over, or cleared. Not known (a day rolled
    // in and never filled): all are carried, and the snapshot is laid out anew at the next bar.
    model::Portfolio* portfolio = structure_.find_portfolio(name);
    const auto found = minutes_.find(portfolio);
    Minute* minute = found != minutes_.end() ? &found->second : nullptr;
    const model::Listing* filled =
        minute != nullptr ? minute->snapshot.shared_listing().get() : nullptr;
    const bool known = minute == nullptr || (filled != nullptr && filled == minute->listed);
    std::vector<std::string> valued;
    if (minute != nullptr && known) {
        for (const std::uint32_t c : minute->chains) {
            valued.push_back(filled->chains()[c].symbol);
        }
    }
    std::vector<std::string> dropped = structure_.replace_contracts(
        name, day.underlying(), day.listing(),
        known ? std::optional<std::span<const std::string>>(valued) : std::nullopt);
    portfolio->set_trading_day(std::chrono::floor<std::chrono::days>(day.day()));
    if (minute != nullptr) {
        if (known) {
            minute->snapshot.relist(day.listing(), minute->chains);
        }
        minute->chains.clear();
        minute->listed = day.listing().get();
    }
    return dropped;
}

void Replay::load(HistoricalData& day, std::span<const std::string> chains) {
    day.load_chains(chains);
    model::Portfolio* portfolio = structure_.find_portfolio(day.portfolio_name());
    if (portfolio == nullptr) {
        return;
    }
    Minute& minute = minutes_[portfolio];
    if (minute.listed != day.listing().get()) {
        minute.chains.clear(); // of another day's
        minute.listed = day.listing().get();
    }
    for (const std::string& symbol : chains) {
        if (const std::optional<std::uint32_t> c = day.listing()->chain(symbol);
            c && !std::ranges::contains(minute.chains, *c)) {
            minute.chains.push_back(*c);
        }
    }
}

void Replay::apply(const std::shared_ptr<const HistoricalData>& day, std::size_t i) {
    if (model::Portfolio* portfolio = structure_.find_portfolio(day->portfolio_name())) {
        Minute& minute = minutes_[portfolio];
        if (minute.listed != day->listing().get()) { // none asked for on this day yet
            minute.chains.clear();
            minute.listed = day->listing().get();
        }
        day->fill(i, minute.snapshot, &minute.chains);
        portfolio->apply(minute.snapshot,
                         std::shared_ptr<const void>(day, &minute.snapshot)); // read where it is
    }
}

} // namespace adapters
