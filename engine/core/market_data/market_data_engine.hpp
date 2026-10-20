#pragma once

/**
 * MarketDataEngine: turns live quotes into the same snapshots a backtest replays (columns in
 * the listing's order, timed by the quotes, not a clock), priced here (pricing::price_snapshot) so
 * the event thread only applies them. Each interval it plans what to stream from what the
 * strategies want (StrategyEngine::wanted): held and ordered options first, then underlyings and
 * subscribed chains nearest the money, within the source's line limit.
 *
 * Threads: start/stop/load_contracts from one control thread; the source's calls only from its
 * own thread, so quotes need no lock.
 */

#include "core/strategy/strategy_engine.hpp"
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
#include <string>
#include <string_view>
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
    /** Asks for the underlying's open, high and low of `day` until now (the open: its first
     * minute's), answered on the source's thread through MarketDataEngine::day_range; a source
     * that cannot ask leaves it unanswered. Any thread. */
    virtual void ask_day_range(const std::string& /*underlying*/,
                               std::chrono::year_month_day /*day*/) {}
};

class MarketDataEngine {
  public:
    using SnapshotSink = std::function<void(const model::PortfolioSnapshot&)>;
    using CloseSink = std::function<void(const std::string& underlying,
                                         std::chrono::year_month_day day, double close)>;
    using DayRangeSink =
        std::function<void(const std::string& underlying, std::chrono::year_month_day day,
                           double open, double high, double low)>;

    /** structure and strategies must outlive the engine. market_data: none in a backtest, or a
     * build without a source. interval: between stream plans. */
    MarketDataEngine(utilities::Logger* logger, const model::Portfolios& structure,
                     const StrategyEngine& strategies, std::unique_ptr<MarketData> market_data,
                     SnapshotSink on_snapshot,
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
    /** After a contract roll: what is wanted is found again by name. Throws while running. */
    void load_contracts();
    /** Where closed() goes; before start. */
    void on_close(CloseSink sink) { on_close_ = std::move(sink); }
    /** Where day_range() goes; before start. */
    void on_day_range(DayRangeSink sink) { on_day_range_ = std::move(sink); }

    // ---- Any thread
    /** What a source logs through. */
    [[nodiscard]] auto log_source() const -> const utilities::LogSource& { return log_; }
    /** The underlying's official close of the day, asked of the source (MarketData::ask_close),
     * the answer to on_close; false when no source runs now (ask again later). */
    bool ask_close(const std::string& underlying, std::chrono::year_month_day day);
    /** The underlying's open, high and low of the day until now, asked of the source
     * (MarketData::ask_day_range), the answer to on_day_range; false when no source runs now. */
    bool ask_day_range(const std::string& underlying, std::chrono::year_month_day day);
    /** The curve it prices with from the next snapshot on (Main, from the values by day each
     * morning); RateCurve's default until one is given. */
    void use_rates(model::RateCurve rates);

    // ---- The source's thread
    /** Held and ordered options (always streamed: holdings are valued and hedged on live prices),
     * then underlyings/hedges, then the chains subscribed (all when max_lines is 0; else strikes
     * nearest the money, split evenly, none until the underlying is priced). A window recenters
     * only once price drifts more than a quarter of it, so streams don't churn. What the day does
     * not list is logged, once a day. */
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
    /** An answer to ask_day_range. */
    void day_range(const std::string& underlying, std::chrono::year_month_day day, double open,
                   double high, double low);

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
        model::PortfolioSnapshot snapshot; // reused: no allocation per publish
    };

    /** One of a book's listing's chains; center -1: not yet. */
    struct Chain {
        std::uint32_t portfolio = 0;
        std::uint32_t index = 0;
        std::ptrdiff_t center = -1;
    };

    /** strikes 0: all. */
    void window(Chain& chain, std::size_t strikes, std::vector<Instrument>& out);
    Quote& at(QuoteSlot slot);
    void mark_changed(QuoteSlot slot);
    /** The day's books and indices, no quotes. */
    void lay_out();
    void reset();

    utilities::LogSource log_;
    const model::Portfolios& structure_;
    const StrategyEngine& strategies_;
    // What live prices with: the curve use_rates gave last, and the mid.
    std::atomic<std::shared_ptr<const model::RateCurve>> rates_{
        std::make_shared<const model::RateCurve>()};
    std::string_view iv_price_mode_ = "mid";
    std::vector<Book> books_;
    std::vector<Chain> chains_;
    std::unordered_map<std::string, std::size_t> chain_index_; // chain symbol → chains_
    std::unordered_map<std::string, QuoteSlot> option_slots_;  // option symbol → slot
    SnapshotSink on_snapshot_;
    CloseSink on_close_;
    DayRangeSink on_day_range_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point next_replan_;

    std::unique_ptr<MarketData> market_data_;
    std::mutex control_mutex_; // start / stop / load_contracts
    std::atomic<bool> running_{false};

    // The source's thread's, or the control thread's while stopped.
    std::shared_ptr<const StrategyEngine::Wanted> planned_; // what the last plan logged
    std::unordered_set<std::string> warned_;               // names the day does not list, logged
};

} // namespace core
