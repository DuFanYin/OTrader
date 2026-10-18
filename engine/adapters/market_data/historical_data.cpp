#include "adapters/market_data/historical_data.hpp"
#include "model/portfolio_spec.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "utilities/fnv.hpp"
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
            expiry_rate_[(i * expiries.size()) + e] = rates_.at(t); // as price_snapshot has it
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
                                              pricing_fingerprint(), risk_free_rate_,
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
            if (!stored.empty()) {
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
    utilities::shared_pool().parallel_for(
        0, index_.times.size(),
        [&](std::size_t i) {
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
            model::price_snapshot(s, rates_, iv_price_mode_);
            std::ranges::copy(s.iv, stored.row(kIv, i).begin());
            stored.set_forward(i, s.forward[0]);
        },
        /*inline_threshold=*/1);
}

void HistoricalData::build_portfolio(model::Portfolios& target) const {
    const std::string& name = index_.portfolio;
    target.ensure_portfolio(name);
    model::Portfolio* port = target.find_portfolio(name);
    if (const model::PortfolioSpec* spec = model::find_portfolio(name)) {
        port->physical = spec->physical;
        if (spec->hedge != spec->underlying) {
            port->hedge_symbol = spec->hedge; // its quotes are not in the day: no hedge
        }
    }
    target.replace_contracts(name, index_.underlying, index_.listing);
    if (index_.day) {
        port->set_trading_day(*index_.day);
    }
    port->set_rates(rates_);
    port->set_iv_price_mode(iv_price_mode_);
}

void HistoricalData::fill(std::size_t i, model::PortfolioSnapshot& minute) const {
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
    for (std::size_t c = 0; c < chains_.size(); ++c) {
        const Chain& chain = *chains_[c];
        if (!chain.loaded.load(std::memory_order_acquire) || !chain.stored) {
            continue;
        }
        const std::uint32_t first = index_.listing->chains()[c].first;
        for (std::size_t q = 0; q < StoredChain::kColumns; ++q) {
            std::ranges::copy(chain.stored->row(q, i),
                              minute.values(option_columns[q]).begin() + first);
        }
        forward[c] = chain.stored->forward(i);
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
                   std::string_view iv_price_mode, const std::string& cache_dir) {
    LoadedDay day = std::make_shared<HistoricalData>();
    day->set_risk_free_rate(risk_free_rate);
    day->set_iv_price_mode(iv_price_mode);
    day->load(parquet_path, cache_dir);
    return day;
}

auto pricing_fingerprint() -> std::uint64_t {
    static const std::uint64_t fingerprint = [] {
        // SPY on 2025-03-03: three expiries, strikes 560 to 600, quotes with holes (none, one
        // side, crossed), at three times, the last with no underlying quote (its mark kept); each
        // IV mode.
        using namespace std::chrono;
        std::vector<model::Contract> contracts;
        for (const year_month_day expiry : {2025y / March / 3d, 2025y / March / 7d,
                                            2025y / March / 21d}) {
            for (double k = 560.0; k <= 600.0; k += 2.5) {
                for (const model::OptionType type : {model::OptionType::CALL,
                                                     model::OptionType::PUT}) {
                    model::Contract c;
                    c.symbol = model::option_symbol(
                        {.root = "SPY", .expiry = expiry, .type = type, .strike = k});
                    c.multiplier = 100.0;
                    contracts.push_back(std::move(c));
                }
            }
        }
        const auto listing = std::make_shared<const model::Listing>(std::move(contracts));
        const sys_days day{2025y / March / 3d};
        const std::array<model::DateTime, 3> times{day + 15h, day + 20h + 30min, day + 20h + 59min};
        utilities::Fnv hash;
        for (const std::string_view mode : {"mid", "bid", "ask"}) {
            for (std::size_t t = 0; t < times.size(); ++t) {
                model::PortfolioSnapshot s(listing, "SPY");
                s.datetime = times[t];
                if (t + 1 < times.size()) {
                    s.underlying_bid = 580.0 + (0.37 * static_cast<double>(t));
                    s.underlying_ask = s.underlying_bid + 0.02;
                }
                s.underlying_mark = 580.5;
                const std::span<double> bid = s.values(&model::Frame::bid);
                const std::span<double> ask = s.values(&model::Frame::ask);
                for (std::size_t i = 0; i < listing->size(); ++i) {
                    if ((i + t) % 7 == 3) {
                        continue; // not quoted
                    }
                    const double k = listing->strike(i);
                    const bool call = listing->type(i) == model::OptionType::CALL;
                    const double value = std::max(call ? 580.0 - k : k - 580.0, 0.0) +
                                         (2.0 + listing->chain_of(i)) *
                                             std::exp(-std::abs(k - 580.0) / 15.0);
                    bid[i] = i % 11 == 5 ? 0.0 : std::max(value - 0.05, 0.01);
                    ask[i] = value + 0.05;
                    if (i % 13 == 6) {
                        std::swap(bid[i], ask[i]); // crossed
                    }
                }
                model::price_snapshot(s, model::RateCurve::flat(0.05), mode);
                for (const auto column : model::PortfolioSnapshot::kOptionColumns) {
                    hash.add(s.*column);
                }
                hash.add(s.forward);
                hash.add(s.expiry_years);
                hash.add(s.priced);
                hash.add(s.greeks_spot);
            }
        }
        return hash.value();
    }();
    return fingerprint;
}

} // namespace adapters
