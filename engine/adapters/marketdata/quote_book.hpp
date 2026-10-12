#pragma once

/**
 * QuoteBook (live): the market data's data plane. It holds the quotes a feed streams, laid out
 * like the snapshots the event engine applies (a portfolio's options in option_apply_order), and
 * after each batch of quotes turns them into a snapshot of each portfolio whose quotes changed:
 * the same snapshots the backtest replays, marked the same way and timed by the quotes, not by a
 * clock, and priced here (PortfolioData::price_snapshot: only the options whose quotes, forward or
 * time moved are priced again), so the event thread only applies them. Once an interval it plans
 * what a feed streams: the options strategies hold, whatever their chain, then the underlyings
 * and the subscribed chains' options nearest the money, within the feed's lines.
 *
 * Threads: subscribe, unsubscribe and hold from any; the rest from one thread at a time (the
 * feed's while it streams), so the quotes need no lock.
 */

#include "../../model/portfolio_structure.hpp"
#include "../../model/object.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace adapters {

/** Where a quote goes: a portfolio's underlying, its hedge instrument, or the option at that
 * position of its option_apply_order, which is the order of a snapshot's columns. */
struct QuoteSlot {
    static constexpr std::int32_t kUnderlying = -1;
    static constexpr std::int32_t kHedge = -2;

    std::uint32_t portfolio = 0;
    std::int32_t option = kUnderlying;

    bool operator==(const QuoteSlot&) const = default;
};

enum class QuoteField : std::uint8_t { Bid, Ask, Last };

/** A contract to stream and the slot its quotes go to. One contract can fill several slots (SPY
 * is the SPY portfolio's underlying and SPXW's hedge): stream it once, quote each. */
struct Instrument {
    const model::ContractData* contract = nullptr;
    QuoteSlot slot;
};

class QuoteBook {
  public:
    using SnapshotSink = std::function<void(const model::PortfolioSnapshot&)>;

    /** Lays the book out from structure, which must be loaded and finalized and outlive the book
     * (instruments point at its contracts; its portfolios price the snapshots). interval: between
     * two plans of what to stream. */
    QuoteBook(const model::PortfolioStructure& structure, SnapshotSink on_snapshot,
              std::chrono::milliseconds interval);

    // ── Subscriptions (any thread)

    /** Stream these chains for strategy_name, in place of what it had; returns the ones the book
     * has no chain for. */
    std::vector<std::string> subscribe(const std::string& strategy_name,
                                       std::span<const std::string> chains);
    void unsubscribe(const std::string& strategy_name);
    /** The options strategies hold (all of them, in place of the last call): they stream whatever
     * else fits, so a holding is always valued and hedged on live prices. Returns the ones the
     * book has no contract for. */
    std::vector<std::string> hold(std::span<const std::string> options);

    // ── The streaming thread

    /** What to stream now: the options held, then the underlying and hedge instrument of each
     * portfolio with a held option or a subscribed chain, and those chains' options. All of them
     * when max_lines is 0; otherwise the held options, and the strikes nearest the underlying's
     * price that fit in the lines left (a contract counted once), shared evenly between the
     * chains, and none until the underlying has a price. A window moves only once the price is
     * more than a quarter of it from its center, so that streams don't churn. */
    std::vector<Instrument> instruments(std::size_t max_lines);
    /** A price, quoted at `time`: when the source says it was, else when it arrived. A snapshot
     * is as of its portfolio's latest quote. */
    void quote(QuoteSlot slot, QuoteField field, double price, model::DateTime time);
    /** Forget a slot's quote: it is no longer streamed. */
    void clear(QuoteSlot slot);
    /** After a batch of quotes: emit a snapshot of each portfolio whose quotes changed since its
     * last one, priced. */
    void publish();
    /** True once an interval: a feed re-plans its streams then. */
    bool replan_due(std::chrono::steady_clock::time_point now);
    /** Forget every quote and window: the next stream starts from nothing. */
    void reset();

  private:
    struct Quote {
        double bid = 0.0;
        double ask = 0.0;
        double last = 0.0;

        double& operator[](QuoteField field);
    };

    /** A portfolio's contracts by column and their quotes. */
    struct Book {
        const model::ContractData* underlying = nullptr;
        const model::ContractData* hedge = nullptr; // when it is not the underlying
        std::vector<const model::ContractData*> options;
        Quote underlying_quote;
        Quote hedge_quote;
        std::vector<Quote> option_quotes;
        model::DateTime quoted_at{}; // the latest quote's time
        bool changed = false;
        // What changed since the last snapshot: what its pricing prices again.
        std::vector<std::uint8_t> option_changed; // by column
        bool underlying_changed = false;
        const model::PortfolioData* portfolio = nullptr; // prices the snapshots
        model::PortfolioSnapshot snapshot; // emitted in place: no allocation per publish
    };

    /** A chain's strikes, ascending, with the column of the call and of the put at each (-1 for
     * none), and where its window is centered (-1: not yet). */
    struct Chain {
        std::uint32_t portfolio = 0;
        std::vector<double> strikes;
        std::vector<std::int32_t> calls;
        std::vector<std::int32_t> puts;
        std::ptrdiff_t center = -1;
    };

    /** The chain's options to stream, `strikes` of them (0: all) around the money, onto out. */
    void window(Chain& chain, std::size_t strikes, std::vector<Instrument>& out);
    Quote& at(QuoteSlot slot);
    /** The slot's quote changed: its book has a snapshot to emit, and that to price again. */
    void mark_changed(QuoteSlot slot);

    std::vector<Book> books_;
    std::vector<Chain> chains_;
    std::unordered_map<std::string, std::size_t> chain_index_; // chain symbol → chains_
    std::unordered_map<std::string, QuoteSlot> option_slots_;  // option symbol → its slot
    SnapshotSink on_snapshot_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point next_replan_;

    std::mutex subscriptions_mutex_;
    std::unordered_map<std::string, std::vector<std::size_t>> subscriptions_; // → chains_
    std::vector<QuoteSlot> held_;
};

} // namespace adapters
