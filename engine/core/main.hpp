#pragma once

#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "core/database/database.hpp"
#include "core/event_loop.hpp"
#include "core/gateway/gateway.hpp"
#include "core/log/log_engine.hpp"
#include "core/market_data/market_data_engine.hpp"
#include "core/risk/risk_engine.hpp"
#include "core/strategy/strategy_engine.hpp"
#include "model/event.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/portfolios.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace core {

/**
 * Main: the glue for backtest and live. Owns the engines, routes each event through them in order,
 * and takes operator commands. Ports (clock, broker, storage, market data) come from the assembly
 * (live::make_main, backtest::Backtest), which holds a Main rather than deriving from it. Operator
 * actions on strategies are persisted so a restart restores them.
 */
class Main final {
  public:
    /** Factories, since adapters need Main's log and portfolios. */
    struct Ports {
        /** None: the latest snapshot's time (backtest). */
        std::function<model::DateTime()> clock;
        std::function<std::unique_ptr<Database>(utilities::Logger*)> database;
        std::function<std::unique_ptr<Gateway>(utilities::Logger*,
                                               const model::Portfolios&)>
            gateway;
        /** None in a backtest or a build without a source. */
        std::unique_ptr<MarketData> market_data;
        /** OPTRA_BROKER_CHECK=warn: strategies start even when the broker disagrees. */
        bool broker_check_warn_only = false;
        /** Reads the values by day its strategies read (data/daily): at the start and at each
         * roll_day, so live sees each close's. None: none. */
        std::function<model::DailyTable()> daily;
        /** None: its own (live). A backtest's is its replay's (backtest::Replay), which rolls each
         * day's contracts in and applies each bar's quotes. */
        model::Portfolios* portfolios = nullptr;
    };

    /** Loads risk limits from storage. Does not restore strategies or start the event loop. */
    explicit Main(Ports ports, utilities::LogLevel log_level = utilities::INFO);
    ~Main();

    Main(const Main&) = delete;
    Main& operator=(const Main&) = delete;

    // ---- Threads. Main's commands (from The broker on) may be called on any thread: what each
    // changes it changes on the event thread, the trading state's one writer, and it returns once
    // done. State is read through read(), on the event thread too. The engines themselves are for
    // the assembly that drives the event loop, and for tests.

    BookkeepingEngine* bookkeeping_engine() { return &bookkeeping_engine_; }
    const BookkeepingEngine* bookkeeping_engine() const { return &bookkeeping_engine_; }
    RiskEngine* risk_engine() { return &risk_engine_; }
    const RiskEngine* risk_engine() const { return &risk_engine_; }
    StrategyEngine* strategy_engine() { return &strategy_engine_; }
    const StrategyEngine* strategy_engine() const { return &strategy_engine_; }
    model::Portfolios* portfolios() { return &portfolios_; }
    const model::Portfolios* portfolios() const { return &portfolios_; }
    EventLoop* event_loop() { return &event_loop_; }
    LogEngine* log_engine() { return &log_; }
    Gateway* gateway() { return gateway_.get(); }
    const Gateway* gateway() const { return gateway_.get(); }
    Database* database() { return database_.get(); }
    MarketDataEngine* market_data() { return &market_data_; }
    const MarketDataEngine* market_data() const { return &market_data_; }

    /** fn(const Main&) on the event thread, from any thread; its result (or its throw). */
    template <typename Fn> auto read(Fn&& fn) {
        return event_loop_.on_event_thread([&] { return fn(std::as_const(*this)); });
    }

    void write_log(const std::string& msg, utilities::LogLevel level = utilities::INFO) const;
    /** Everything takes time from here, never the system clock, so backtests replay exactly. */
    model::DateTime now() const { return clock_ ? clock_() : snapshot_time_; }

    // ---- The event thread's: the event loop's handler and the assembly call these there

    /** Event loop latencies and strategy callback times. */
    [[nodiscard]] std::string latency_report() const;
    void process_event(const model::Event& event);
    /** The quotes into their portfolio, which takes over the snapshot's values (leaving it a buffer
     * to reuse), then snapshot_applied (live). An unknown portfolio's only moves the time. */
    void apply_snapshot(model::PortfolioSnapshot& snapshot);
    /** The frame's quotes are in its portfolio (a backtest applies them itself): the engine's
     * time advances and the strategies are told. */
    void snapshot_applied(const model::Frame& frame);
    /** For assemblies whose storage outlives them: the strategies saved, hosted again. */
    void restore();
    /** The values by day the strategies and the risk checks read; it must outlive them (or set
     * before the event loop runs). */
    void use_daily(const model::DailyTable* daily);
    const model::Portfolio* find_portfolio(const std::string& name) const;
    const model::Contract* find_contract(const std::string& symbol) const;
    /** An order of the strategy's (StrategyEngine::Callbacks::send_order), or the engine's for it:
     * risk-checked, then tracked as the strategy's once accepted; its reference, when it has none,
     * Strategy_<name>. Empty when refused or rejected. */
    std::string send_order(const model::StrategyRecord& strategy, model::OrderRequest req);
    void cancel_order(const model::CancelRequest& req);

    // ---- The broker

    /** With a broker, also catches up and checks the positions with it (failures are logged and
     * block strategy starts). Throws when unreachable. */
    void connect();
    /** Saves them; does not roll them in (roll_day does both). Blocks. */
    std::vector<model::Contract> sync_contracts(const std::string& portfolio, int max_dte);
    /** Replaces the day's contracts on the event thread; held options stay even if unlisted. DTE
     * counts from `day` (00:00 UTC), then strategies pick chains. Live, market data is stopped
     * meanwhile since it prices the portfolio on its own thread. */
    void roll_contracts(const std::string& portfolio,
                        std::span<const model::Contract> contracts, model::DateTime day);
    /** roll_contracts after the roll, done by another (a backtest's replay, which dropped these
     * chains). */
    void contracts_rolled(const std::string& portfolio, std::span<const std::string> dropped);
    /** Portfolio name, max DTE to sync. */
    using Synced = std::pair<std::string, int>;
    /** Sync and roll in, serialized. Throws if a sync fails, leaving the old contracts. */
    std::vector<model::Contract> roll_day(std::span<const Synced> portfolios);

    // ---- Strategies (operator actions are persisted)

    /** Returns the name; names are never reused (history is kept by name). */
    std::string add_strategy(const std::string& class_name, const std::string& portfolio,
                             const model::Values& setting, const std::string& strategy_name = {});
    void init_strategy(const std::string& name);
    /** With a broker, the broker check must have run and agreed (unless warn-only). */
    void start_strategy(const std::string& name);
    void stop_strategy(const std::string& name);
    /** restore_strategy brings it back. */
    void remove_strategy(const std::string& name);
    /** Permanent; its orders and fills stay, the name stays taken. */
    void delete_strategy(const std::string& name);
    /** Back stopped, holding rebooked from its fills. */
    void restore_strategy(const std::string& name);
    /** Failures are logged. */
    void start_strategies_that_were_running();

    // ---- Risk

    /** Cancels working orders, then stops every running strategy (what they close as they stop
     * goes): only reducing orders until clear_halt(). Persists across restarts. */
    void halt(const std::string& reason);
    void clear_halt();
    /** Throws on an unknown name or out-of-range value; saved. */
    void set_risk_settings(const std::unordered_map<std::string, double>& values);

    // ---- Broker check and expiry

    /** Queries the account and records the check. Throws when the broker cannot be asked. */
    std::vector<BrokerCheckRow> check_with_broker();
    std::vector<BrokerCheckRow> check_with_broker(const BrokerAccount& account);
    /** Read-only. */
    std::vector<BrokerCheckRow> compare_with_broker(const BrokerAccount& account);
    /** Corrects a position the engine missed when the broker is right; saved and replayed like
     * any fill. symbol: listed or held (an expired option is no longer listed). Returns the id,
     * manual-<strategy>-<symbol>-<epoch ms>. Throws unless quantity > 0, price >= 0 and a reason is
     * given. */
    std::string book_manually(const std::string& strategy_name, const std::string& symbol,
                              model::Side side, double quantity, double price,
                              const std::string& reason);
    /** Settlement by hand of what the timer could not settle (BookkeepingEngine::unsettled_options;
     * expiry is a New York date). Returns the number of settlements booked. */
    int settle_expired(const std::string& portfolio, std::chrono::year_month_day expiry,
                       double price);

    // ---- Storage

    /** what: "trading" or "contracts"; returns rows deleted per table. "trading" is refused while
     * positions or working orders exist, since a restart would lose them. */
    std::vector<std::pair<std::string, int>> wipe_database(const std::string& what);

    /** Reverse of construction; idempotent. Strategies are dropped, not stopped, so they resume
     * from storage. Stop TradingDays first. */
    void close();

  private:
    // ---- Booking: the bookkeeping, then storage, then the strategy

    /** Merged into the book; an end waits for its fills (BookkeepingEngine::update_order). */
    void book_order(const model::Order& order);
    /** The order as the book now has it: saved, and told to its strategy. */
    void order_changed(const model::Order& order);
    /** Drops repeated tradeids (IB replays the day's fills on reconnect). A fill of an order no
     * strategy sent is held by none. */
    void book_fill(const model::Fill& fill);
    /** For settlements and manual fills. False (ERROR) and nothing booked if the id exists. */
    bool book_filled_order(const std::string& strategy_name, const model::Order& order,
                           std::span<const model::Fill> fills);
    /** The strategy's options expiring then, settled at `price`; returns the number booked. */
    int settle(const std::string& strategy_name, std::chrono::year_month_day expiry, double price,
               const model::Portfolio& portfolio);
    /** Replays saved orders and fills without notifying the strategies. Event thread. */
    void rebook(const std::unordered_set<std::string>& strategies);

    // ---- Each bar

    /** Settles expired options first so the strategies, metrics and hedges see what is left. */
    void on_timer(model::DateTime now);
    /** Settles at the underlying's closing price where the portfolio has it. */
    void settle_due(model::DateTime now);
    /** Closes share-settled options in their flatten window. */
    void flatten_expiring(model::DateTime now);
    /** Settles what expired on `day` at the underlying's close the source gave (event thread). */
    void settle_at(const std::string& underlying, std::chrono::year_month_day day, double close);

    void save_risk();
    /** Market data first, storage last. */
    void close_ports();

    // In dependency order (built top down, destroyed bottom up); ports come from the constructor.
    mutable LogEngine log_;
    model::Portfolios own_portfolios_; // unused when given one
    model::Portfolios& portfolios_;
    std::function<model::DateTime()> clock_;
    std::unique_ptr<Database> database_;
    EventLoop event_loop_{&log_, [this](const model::Event& e) { process_event(e); },
                          [this] { return latency_report(); }};
    std::unique_ptr<Gateway> gateway_;
    BookkeepingEngine bookkeeping_engine_{&log_};
    RiskEngine risk_engine_{&log_, portfolios_, bookkeeping_engine_};
    MarketDataEngine market_data_;
    std::function<model::DailyTable()> read_daily_;
    std::shared_ptr<const model::DailyTable> daily_; // what strategy_engine_ reads
    // The closes asked of the source (settle_due), by underlying and day: once each.
    std::set<std::pair<std::string, std::chrono::year_month_day>> asked_closes_;
    StrategyEngine strategy_engine_{
        &log_,
        portfolios_,
        bookkeeping_engine_,
        market_data_,
        {.send_order = [this](const model::StrategyRecord& strategy,
                              const model::OrderRequest& req) { return send_order(strategy, req); },
         .cancel_order = [this](const model::CancelRequest& req) { cancel_order(req); },
         .state_changed = [this](const model::StrategyRecord& r) { database_->save_strategy(r); }}};

    model::DateTime snapshot_time_{}; // the backtest's clock
    std::mutex roll_mutex_;
    std::mutex connect_mutex_; // the trading days' connect and the operator's
    const bool broker_check_warn_only_;
    bool closed_ = false;
};

} // namespace core
