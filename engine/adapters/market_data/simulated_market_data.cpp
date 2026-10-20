#include "adapters/market_data/simulated_market_data.hpp"
#include "adapters/market_data/data_loader.hpp"
#include "model/portfolio_spec.hpp"
#include "model/symbol.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace adapters {

SimulatedMarketData::SimulatedMarketData(std::vector<std::string> files, Clock clock,
                       std::function<void()> after_minute)
    : files_(std::move(files)), clock_(std::move(clock)), after_minute_(std::move(after_minute)) {
    load();
}

SimulatedMarketData::~SimulatedMarketData() { stop(); }

void SimulatedMarketData::load() {
    for (const std::string& file : files_) {
        auto read = read_parquet_quotes(file);
        if (!read) {
            throw std::runtime_error(std::format("cannot read {}: {}", file, read.error()));
        }
        const ParquetQuotes& quotes = *read;
        const std::string portfolio = portfolio_of_file(file);
        std::vector<std::string> names; // per code the platform's name; empty: not an option
        names.reserve(quotes.codes.size());
        for (const std::string& code : quotes.codes) {
            const std::optional<model::OptionId> id = model::parse_occ_code(portfolio, code);
            names.push_back(id ? model::option_symbol(*id) : std::string());
        }
        for (const ParquetQuotes::Minute& at : quotes.minutes) {
            Minute minute;
            minute.time = at.time;
            minute.underlying = model::underlying_of(portfolio);
            minute.options.reserve(at.end - at.first);
            for (std::size_t row = at.first; row < at.end; ++row) {
                // The underlying's last quote in the minute, as a backtest day takes it.
                if (!std::isnan(quotes.underlying_bid[row])) {
                    minute.underlying_bid = quotes.underlying_bid[row];
                }
                if (!std::isnan(quotes.underlying_ask[row])) {
                    minute.underlying_ask = quotes.underlying_ask[row];
                }
                const std::string& name = names[quotes.code[row]];
                if (name.empty()) {
                    continue;
                }
                minute.options.push_back(name);
                minute.bids.push_back(quotes.bid[row]);
                minute.asks.push_back(quotes.ask[row]);
                minute.bid_sizes.push_back(quotes.bid_size[row]);
                minute.ask_sizes.push_back(quotes.ask_size[row]);
            }
            minutes_.push_back(std::move(minute));
        }
    }
    std::ranges::stable_sort(minutes_, {}, &Minute::time);
}

void SimulatedMarketData::start(core::MarketDataEngine& engine) {
    stop();
    {
        const std::lock_guard lock(mutex_);
        stopping_ = false;
    }
    engine.log_source().write_log(std::format("Simulated quotes: {} minutes from {} files",
                                              minutes_.size(), files_.size()),
                                  utilities::INFO);
    thread_ = std::thread([this, &engine] { run(engine); });
}

void SimulatedMarketData::stop() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SimulatedMarketData::run(core::MarketDataEngine& engine) {
    // Each contract the engine plans, by name, and the slots its quotes go to.
    std::unordered_map<std::string, std::vector<core::QuoteSlot>> slots;
    auto plan = [&] {
        for (auto& [_, s] : slots) {
            for (const core::QuoteSlot& slot : s) {
                engine.clear(slot);
            }
        }
        slots.clear();
        for (const core::Instrument& i : engine.instruments(0)) {
            slots[i.contract->symbol].push_back(i.slot);
        }
    };
    plan();
    std::size_t next = 0;
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        lock.unlock();
        if (engine.replan_due(std::chrono::steady_clock::now())) {
            plan();
        }
        bool published = false;
        const model::DateTime now = clock_();
        while (next < minutes_.size() && minutes_[next].time <= now) {
            const Minute& m = minutes_[next++];
            if (auto it = slots.find(m.underlying); it != slots.end()) {
                for (const core::QuoteSlot& slot : it->second) {
                    engine.quote(slot, core::QuoteField::Bid, m.underlying_bid, m.time);
                    engine.quote(slot, core::QuoteField::Ask, m.underlying_ask, m.time);
                }
            }
            for (std::size_t k = 0; k < m.options.size(); ++k) {
                if (auto it = slots.find(m.options[k]); it != slots.end()) {
                    for (const core::QuoteSlot& slot : it->second) {
                        engine.quote(slot, core::QuoteField::BidSize, m.bid_sizes[k], m.time);
                        engine.quote(slot, core::QuoteField::AskSize, m.ask_sizes[k], m.time);
                        engine.quote(slot, core::QuoteField::Bid, m.bids[k], m.time);
                        engine.quote(slot, core::QuoteField::Ask, m.asks[k], m.time);
                    }
                }
            }
            published = true;
        }
        if (published) {
            engine.publish();
            if (after_minute_) {
                after_minute_();
            }
        }
        lock.lock();
        if (!asked_.empty()) {
            const auto asked = std::exchange(asked_, {});
            lock.unlock();
            answer(engine, asked);
            lock.lock();
        }
        wake_.wait_for(lock, std::chrono::milliseconds(250),
                       [this] { return stopping_ || !asked_.empty(); });
    }
}

void SimulatedMarketData::ask_close(const std::string& underlying,
                                    std::chrono::year_month_day day) {
    {
        const std::lock_guard lock(mutex_);
        asked_.push_back({.underlying = underlying, .day = day});
    }
    wake_.notify_all();
}

void SimulatedMarketData::ask_day_range(const std::string& underlying,
                                        std::chrono::year_month_day day) {
    {
        const std::lock_guard lock(mutex_);
        asked_.push_back({.underlying = underlying, .day = day, .range = true});
    }
    wake_.notify_all();
}

/** The underlying's mid at the day's first minute, the highest and lowest of its minutes until `now`
 * (all of them when it is none); none without a quote. */
static auto day_range(const ParquetQuotes& q, model::DateTime now)
    -> std::optional<std::array<double, 3>> {
    std::optional<std::array<double, 3>> out; // open, high, low
    for (const ParquetQuotes::Minute& m : q.minutes) {
        if (now != model::DateTime{} && m.time > now) {
            break;
        }
        if (m.first == m.end || !std::isfinite(q.underlying_bid[m.first]) ||
            !std::isfinite(q.underlying_ask[m.first])) {
            continue;
        }
        const double mid = 0.5 * (q.underlying_bid[m.first] + q.underlying_ask[m.first]);
        if (!out) {
            out = std::array{mid, mid, mid};
        }
        (*out)[1] = std::max((*out)[1], mid);
        (*out)[2] = std::min((*out)[2], mid);
    }
    return out;
}

void SimulatedMarketData::answer(core::MarketDataEngine& engine,
                                 const std::vector<Asked>& asked) {
    for (const auto& [underlying, day, range] : asked) {
        for (const std::string& file : files_) {
            const std::string portfolio = portfolio_of_file(file);
            if (model::underlying_of(portfolio) != underlying) {
                continue;
            }
            const std::chrono::sys_days on{day};
            const std::filesystem::path that = std::filesystem::path(file).parent_path().parent_path() /
                                               std::format("{}-{:%Y-%m}", portfolio, on) /
                                               std::format("{:%Y%m%d}.parquet", on);
            auto read = read_parquet_quotes(that.string());
            if (!read) {
                engine.log_source().write_log(
                    std::format("No {} of {} on {}: {}", range ? "range" : "close", underlying,
                                on, read.error()),
                    utilities::WARNING);
                break;
            }
            const ParquetQuotes& q = *read;
            if (range) {
                if (const auto found = day_range(q, clock_())) {
                    engine.day_range(underlying, day, (*found)[0], (*found)[1], (*found)[2]);
                }
                break;
            }
            // The last minute that quotes the underlying on both sides.
            for (std::size_t row = q.underlying_bid.size(); row-- > 0;) {
                if (std::isfinite(q.underlying_bid[row]) && std::isfinite(q.underlying_ask[row])) {
                    engine.closed(underlying, day,
                                  0.5 * (q.underlying_bid[row] + q.underlying_ask[row]));
                    break;
                }
            }
            break;
        }
    }
}

} // namespace adapters
