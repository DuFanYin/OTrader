#pragma once

/**
 * MarketDataEngine: turns live quotes into the same snapshots a backtest replays (columns in
 * option_apply_order, timed by the quotes, not a clock), priced here so the event thread only
 * applies them. Each interval it plans what to stream: held options first, then underlyings and
 * subscribed chains nearest the money, within the source's line limit.
 *
 * Threads: start/stop/load_contracts from one control thread; subscribe/hold/ordered from any;
 * the source's calls only from its own thread, so quotes need no lock.
 */

#include "model/object.hpp"
#include "model/portfolios.hpp"
#include "utilities/log_source.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace core {

/** option: its column (the portfolio's listing's, the snapshot's), or kUnderlying / kHedge. */
struct QuoteSlot {
    static constexpr std::int32_t kUnderlying = -1;
    static constexpr std::int32_t kHedge = -2;

    std::uint32_t portfolio = 0;
    std::int32_t option = kUnderlying;

    bool operator==(const QuoteSlot&) const = default;
};

enum class QuoteField : std::uint8_t { Bid, Ask, Last, BidSize, AskSize };

/** One contract can fill several slots (SPY is SPY's underlying and SPXW's hedge): stream it
 * once, quote each. */
struct Instrument {
    const model::Contract* contract = nullptr;
    QuoteSlot slot;
};

class MarketDataEngine;

/** The live quote source port. Streams on its own thread what instruments() plans, calling
 * quote/clear and publish after each batch. */
class MarketData {
  public:
    virtual ~MarketData() = default;

    /** Throws when the source cannot be reached. */
    virtual void start(MarketDataEngine& engine) = 0;
    /** Nothing touches the engine after it returns. */
    virtual void stop() = 0;
    /** Asks for the underlying's official close on `day`, answered on the source's thread through
     * MarketDataEngine::closed; a source that cannot ask leaves it unanswered. Any thread. */
    virtual void ask_close(const std::string& /*underlying*/, std::chrono::year_month_day /*day*/) {}
};

class MarketDataEngine {
  public:
    using SnapshotSink = std::function<void(const model::PortfolioSnapshot&)>;
    using CloseSink = std::function<void(const std::string& underlying,
                                         std::chrono::year_month_day day, double close)>;

    /** structure must outlive the engine. market_data: none in a backtest, or a build without a
     * source. interval: between stream plans. */
    MarketDataEngine(utilities::Logger* logger, const model::Portfolios& structure,
                     std::unique_ptr<MarketData> market_data, SnapshotSink on_snapshot,
                     std::chrono::milliseconds interval = std::chrono::seconds(1));
    ~MarketDataEngine();

    MarketDataEngine(const MarketDataEngine&) = delete;
    MarketDataEngine& operator=(const MarketDataEngine&) = delete;

    // ---- Control thread
    /** Throws without a source. */
    void start();
    /** Also forgets quotes: a restart begins from nothing. */
    void stop();
    void close() { stop(); }
    [[nodiscard]] bool running() const { return running_; }
    /** After a contract roll; subscriptions are kept by name. Throws while running. */
    void load_contracts();
    /** Where closed() goes; before start. */
    void on_close(CloseSink sink) { on_close_ = std::move(sink); }

    // ---- Any thread
    /** What a source logs through. */
    [[nodiscard]] auto log_source() const -> const utilities::LogSource& { return log_; }
    /** Replaces the strategy's chains; unknown ones are logged. */
    void subscribe_chains(const std::string& strategy_name, std::span<const std::string> chains);
    void unsubscribe_chains(const std::string& strategy_name);
    /** The underlying's official close of the day, asked of the source (MarketData::ask_close),
     * the answer to on_close; false when no source runs now (ask again later). */
    bool ask_close(const std::string& underlying, std::chrono::year_month_day day);
    /** The options held, replacing the last call. */
    void set_held_options(std::span<const std::string> options);
    /** Its options stream for the rest of the day: the risk check and the fill read them. */
    void add_ordered_options(const model::OrderRequest& request);
    /** At a contract roll: the day before's orders are done. Streams change with the next call. */
    void clear_ordered_options();
    std::unordered_map<std::string, std::vector<std::string>> subscribed_chains() const;
    /** Held plus ordered today, sorted. Always streamed, so holdings are valued and hedged on live
     * prices. */
    std::vector<std::string> held_and_ordered_options() const;
    /** The chains whose quotes are wanted: every strategy's subscribed, and those of the options
     * held or ordered today; sorted. What a backtest's day loads (HistoricalData::load_chains). */
    std::vector<std::string> chains_wanted() const;
    /** How many times what is wanted may have changed: a new count, chains_wanted() again. */
    [[nodiscard]] std::uint64_t wanted_changes() const {
        return wanted_changes_.load(std::memory_order_acquire);
    }

    // ---- The source's thread
    /** Held options, then underlyings/hedges, then chains (all when max_lines is 0; else strikes
     * nearest the money, split evenly, none until the underlying is priced). A window recenters
     * only once price drifts more than a quarter of it, so streams don't churn. */
    std::vector<Instrument> instruments(std::size_t max_lines);
    /** time: the source's timestamp, else arrival. A size alone triggers no snapshot. */
    void quote(QuoteSlot slot, QuoteField field, double value, model::DateTime time);
    void clear(QuoteSlot slot);
    /** Emits a priced snapshot of each changed portfolio. */
    void publish();
    /** True once per interval. */
    bool replan_due(std::chrono::steady_clock::time_point now);
    /** An answer to ask_close. */
    void closed(const std::string& underlying, std::chrono::year_month_day day, double close);

  private:
    struct Quote {
        double bid = 0.0;
        double ask = 0.0;
        double last = 0.0;
        double bid_size = 0.0;
        double ask_size = 0.0;

        double& operator[](QuoteField field);
    };

    struct Book {
        const model::Contract* underlying = nullptr;
        const model::Contract* hedge = nullptr; // when not the underlying
        std::shared_ptr<const model::Listing> listing; // its options, by column
        Quote underlying_quote;
        Quote hedge_quote;
        std::vector<Quote> option_quotes;
        model::DateTime quoted_at{};
        bool changed = false;
        std::uint64_t changed_tsc = 0; // first change since the last snapshot (TSC)
        // Changed since the last snapshot: what gets repriced.
        std::vector<std::uint8_t> option_changed; // by column
        bool underlying_changed = false;
        const model::Portfolio* portfolio = nullptr; // prices the snapshots
        model::PortfolioSnapshot snapshot; // reused: no allocation per publish
    };

    /** One of a book's listing's chains; center -1: not yet. */
    struct Chain {
        std::uint32_t portfolio = 0;
        std::uint32_t index = 0;
        std::ptrdiff_t center = -1;
    };

    /** Found indices, and the names not found. */
    std::pair<std::vector<std::size_t>, std::vector<std::string>>
    resolve_chains(std::span<const std::string> chains) const;
    std::pair<std::vector<QuoteSlot>, std::vector<std::string>>
    resolve_options(std::span<const std::string> options) const;
    /** strikes 0: all. */
    void window(Chain& chain, std::size_t strikes, std::vector<Instrument>& out);
    Quote& at(QuoteSlot slot);
    void mark_changed(QuoteSlot slot);
    /** Forgets quotes; re-resolves subscriptions by name. */
    void lay_out();
    void reset();
    /** Streams held_and_ordered_options() if they changed. */
    void subscriptions_changed();
    /** held_and_ordered_options(), subscriptions_mutex_ held by the caller. */
    std::vector<std::string> held_and_ordered_options_locked() const;

    utilities::LogSource log_;
    const model::Portfolios& structure_;
    std::vector<Book> books_;
    std::vector<Chain> chains_;
    std::unordered_map<std::string, std::size_t> chain_index_; // chain symbol → chains_
    std::unordered_map<std::string, QuoteSlot> option_slots_;  // option symbol → slot
    SnapshotSink on_snapshot_;
    CloseSink on_close_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point next_replan_;

    std::unique_ptr<MarketData> market_data_;
    std::mutex control_mutex_; // start / stop
    std::atomic<bool> running_{false};

    mutable std::mutex subscriptions_mutex_;
    // Requests by name (survive load_contracts), and their resolved places.
    std::unordered_map<std::string, std::vector<std::string>> subscribed_;
    std::vector<std::string> held_options_;
    std::unordered_set<std::string> ordered_options_;
    std::vector<std::string> streamed_options_; // held_and_ordered_options() as last streamed
    std::unordered_map<std::string, std::vector<std::size_t>> subscriptions_; // → chains_
    std::vector<QuoteSlot> streamed_;
    std::atomic<std::uint64_t> wanted_changes_{0};
};

} // namespace core
