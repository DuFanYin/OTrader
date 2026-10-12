#pragma once

/**
 * A simulated market for the live engine (entry_simulate): recorded days of quotes, the
 * backtest's Parquet files, replayed as a live market, and a venue that fills at them. ReplayFeed
 * streams each minute's quotes once the engine's clock reaches it; SimulatedGateway fills orders at
 * those quotes as the backtest does (bought at the ask, sold at the bid) and keeps the account the
 * fills make. No broker is behind it: nothing to catch up on or check the holdings against. A
 * file's portfolio is its folder's: data/<PORTFOLIO>/<PORTFOLIO>-YYYY-MM/YYYYMMDD.parquet.
 */

#include "../marketdata/market_data_feed.hpp"
#include "engine_gateway_backtest.hpp"
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace adapters {

/** A portfolio's contracts as a broker's sync would give them: its underlying and every option the
 * files of that portfolio quote. */
std::vector<model::ContractData> simulated_contracts(std::span<const std::string> files,
                                                     std::string_view portfolio);

/** The portfolios the files are of, each once. */
std::vector<std::string> simulated_portfolios(std::span<const std::string> files);

/** The first and the last time the files quote: the session the replay runs. Throws when a file
 * cannot be read or quotes nothing. */
std::pair<model::DateTime, model::DateTime> simulated_session(std::span<const std::string> files);

/** The files' quotes, a minute at a time, as the clock reaches each minute. */
class ReplayFeed final : public MarketDataFeed {
  public:
    using Clock = std::function<model::DateTime()>;

    /** after_minute: called on the feed's thread once each batch of minutes is published. */
    ReplayFeed(std::vector<std::string> files, Clock clock, std::function<void()> after_minute);
    ~ReplayFeed() override;

    void start(QuoteBook& book, const utilities::BaseEngine& log) override;
    void stop() override;

  private:
    /** One minute of one portfolio: its underlying's quote and its options'. */
    struct Minute {
        model::DateTime time{};
        std::string underlying;
        double underlying_bid = 0.0;
        double underlying_ask = 0.0;
        std::vector<std::string> options; // the platform's names
        std::vector<double> bids;
        std::vector<double> asks;
    };

    void load();
    void run(QuoteBook& book);

    std::vector<std::string> files_;
    Clock clock_;
    std::function<void()> after_minute_;
    std::vector<Minute> minutes_; // every portfolio's, in time order
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread thread_;
};

/** BacktestGateway's fills, live: an order is filled at once when the quotes allow, a waiting one
 * each minute after (on_minute). The account is what the fills made, from a starting cash: kept in
 * account_file, so a restart goes on from it. */
class SimulatedGateway final : public BacktestGateway {
  public:
    using Clock = std::function<model::DateTime()>;

    static constexpr double kStartingCash = 100'000.0;

    SimulatedGateway(const model::PortfolioStructure& portfolios, std::vector<std::string> files,
                     std::string account_file, Clock clock, double fee_rate, double slippage_bps);

    void connect() override;
    void disconnect() override { connected_ = false; }
    bool is_connected() const override { return connected_; }
    /** Queued, then filled at once if the quotes allow. */
    std::string send_order(const model::OrderRequest& req) override;
    /** Orders from before a restart: the simulated venue kept none, so they are cancelled. */
    void restore_orders(std::span<const model::OrderData> orders) override;
    std::future<BrokerAccount> query_account() override;
    std::future<std::vector<model::ContractData>> sync_contracts(std::string_view portfolio,
                                                                 int max_dte) override;

    /** Each minute (event thread): the waiting orders the new quotes allow, the account's prices,
     * and its options past their close settled at the underlying's price. */
    void on_minute();

  private:
    struct Position {
        double quantity = 0.0; // signed: shares, and options in contracts
        double cost = 0.0;     // what the open quantity cost, in USD
        double realized = 0.0;
    };

    /** A fill into the account (the fill callback's, ahead of the engine's). */
    void book(const std::string& symbol, double quantity, double price);
    void settle_expired();
    double mid(const std::string& symbol) const; // event thread: the portfolios' quotes
    void load_account();
    void save_account() const;

    const model::PortfolioStructure& portfolios_;
    std::vector<std::string> files_;
    std::string account_file_;
    Clock clock_;
    bool connected_ = false;
    bool booking_ = false; // the fill callback wraps the engine's once

    mutable std::mutex mutex_; // the account: written on the event thread, read by query_account
    double cash_ = kStartingCash;
    double fees_booked_ = 0.0;
    std::map<std::string, Position> positions_;
    std::map<std::string, double> marks_; // the latest mid of each contract held
};

} // namespace adapters
