#pragma once

/**
 * MarketDataEngine (live): the market data's control plane. It owns the quote book (the data
 * plane, quote_book.hpp) and the feed that fills it, starts and stops the feed, and takes the
 * chains strategies subscribe. Whatever the source, the event engine gets the same
 * PortfolioSnapshots the backtest replays. It can record them (record_to): each portfolio as its
 * quotes stood at the end of each minute, a file a day, which a backtest replays once converted
 * (app/server/optra/services/recorded_days.py; POST /api/data/recorded).
 */

#include "../../model/portfolio_structure.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/log_level.hpp"
#include "market_data_feed.hpp"
#include "quote_book.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adapters {

class MarketDataEngine : public utilities::BaseEngine {
  public:
    /** The book is laid out from structure (see QuoteBook); interval: between two plans of what
     * to stream. feed may be null, in a build without a source; start() then throws. */
    MarketDataEngine(utilities::Logger* logger, const model::PortfolioStructure& structure,
                     std::unique_ptr<MarketDataFeed> feed, QuoteBook::SnapshotSink on_snapshot,
                     std::chrono::milliseconds interval = std::chrono::seconds(1));
    ~MarketDataEngine();

    MarketDataEngine(const MarketDataEngine&) = delete;
    MarketDataEngine& operator=(const MarketDataEngine&) = delete;

    void start();
    /** Stop the feed and forget its quotes: a restart begins from nothing. */
    void stop();
    void close() { stop(); }
    [[nodiscard]] bool running() const { return running_; }

    /** Stream these chains for strategy_name, in place of what it had; unknown ones are logged. */
    void subscribe_chains(const std::string& strategy_name, std::span<const std::string> chains);
    void unsubscribe_chains(const std::string& strategy_name);
    /** The options strategies hold: always streamed (QuoteBook::hold). */
    void hold(std::span<const std::string> options);

    /** The book the feed streams into (tests read what it plans). */
    QuoteBook& book() { return book_; }

    /** Record the snapshots under dir: each portfolio as its quotes stood at the end of each
     * minute (New York), a CSV a day (dir/<portfolio>/<portfolio>-<YYYY-MM>/<YYYYMMDD>.csv: the
     * time in ns, the option as the backtest data names it, bid, ask, the underlying's bid and
     * ask). Written on a thread of its own. Call before start(). */
    void record_to(const std::string& dir);

  private:
    /** The snapshot as it passes (the feed's thread): kept as its minute's latest; a later
     * minute hands the one before to the recorder thread. */
    void record(const model::PortfolioSnapshot& snapshot);
    /** Hand every minute kept to the recorder thread (stop()). */
    void hand_over_all();
    void run_recorder(const std::stop_token& st);
    void write_minute(const model::DateTime& minute_end, const model::PortfolioSnapshot& s);

    const model::PortfolioStructure& structure_;
    QuoteBook book_;
    std::unique_ptr<MarketDataFeed> feed_;
    std::mutex control_mutex_; // start / stop
    std::atomic<bool> running_{false};

    // Recording (record_to): the feed's thread keeps each portfolio's minute (its end, the
    // snapshot); the minutes done go to the recorder thread.
    std::string record_dir_;
    std::unordered_map<std::string, std::vector<std::string>> record_symbols_; // by column
    std::unordered_map<std::string, std::pair<model::DateTime, model::PortfolioSnapshot>> minute_;
    std::mutex recorded_mutex_;
    std::condition_variable_any recorded_cv_;
    std::vector<std::pair<model::DateTime, model::PortfolioSnapshot>> recorded_;
    std::jthread recorder_;
};

} // namespace adapters
