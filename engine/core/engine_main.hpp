#pragma once

#include "core/engines/execution.hpp"
#include "core/engines/hedge.hpp"
#include "core/engines/log.hpp"
#include "core/engines/option_strategy.hpp"
#include "core/engines/position.hpp"
#include "core/engines/risk.hpp"
#include "core/runtime_api.hpp"
#include "model/portfolio_structure.hpp"
#include "adapters/db/engine_db_sqlite.hpp"
#include "adapters/engine_gateway.hpp"
#include "adapters/marketdata/engine_market_data.hpp"
#include "utilities/base_engine.hpp"
#include "model/event.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "core/engine_event.hpp"
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace core {

/**
 * The engine, backtest and live alike: the core engines (strategies, execution, positions,
 * hedges) and what each event does to them, around five ports:
 *   - the event loop (EventEngine): how the events arrive, and on which thread;
 *   - the clock: the data's time in a backtest, the wall clock live;
 *   - the venue (adapters::Gateway): where orders go and fills come from;
 *   - the storage (adapters::DatabaseEngine): strategies, orders, fills and holdings, in a file live
 *     and in memory in a backtest;
 *   - the market data (adapters::MarketDataEngine): live quotes as snapshots, live only (a
 *     backtest replays its own).
 * An assembly (core::backtest::MainEngine, core::live::MainEngine) sets the ports, then
 * init_core(). Nothing here is virtual but what the core engines call (core::RuntimeAPI): an
 * assembly only sets the ports and adds its own operations. What the operator does to a strategy is kept in the storage, so that a restart
 * brings it back as it was (restore); a strategy starts only once its positions agree with the
 * broker's (PositionEngine::start_refusal); options held past their expiry are settled at the
 * close (settle_due).
 */
class Engine : public core::RuntimeAPI {
  public:
    Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- Core engine accessors ----
    core::ExecutionEngine* execution_engine() { return execution_engine_.get(); }
    core::PositionEngine* position_engine() { return position_engine_.get(); }
    core::LogEngine* log_engine() { return log_engine_.get(); }
    core::OptionStrategyEngine* option_strategy_engine() { return option_strategy_engine_.get(); }
    const core::OptionStrategyEngine* option_strategy_engine() const {
        return option_strategy_engine_.get();
    }
    model::PortfolioStructure* portfolio_structure() { return portfolio_structure_.get(); }
    const model::PortfolioStructure* portfolio_structure() const {
        return portfolio_structure_.get();
    }
    core::HedgeEngine* hedge_engine() { return hedge_engine_.get(); }
    core::RiskEngine* risk_engine() { return risk_engine_.get(); }
    EventEngine* event_engine() { return event_engine_.get(); }

    /** Log msg as "Main". */
    void write_log(const std::string& msg, utilities::LogLevel level = utilities::INFO) const;
    void set_log_level(utilities::LogLevel level);
    utilities::LogLevel log_level() const;

    // ---- The clock and the events ----

    /** The engine's time, from the clock port. Core and runtime take the time from here, never
     * from the system clock. */
    model::DateTime now() const { return clock_(); }
    /** Bring an event in, through the event loop. */
    void put_event(const model::Event& e);
    /** A portfolio's quotes, through the event loop: live, only the newest not yet applied is
     * kept (EventEngine::put_snapshot). */
    void put_snapshot(const model::PortfolioSnapshot& snapshot);
    model::PortfolioSnapshot* acquire_snapshot();
    model::OrderData* acquire_order();
    model::TradeData* acquire_trade();
    /** What an event does (event thread): a snapshot updates its portfolio (its strategies'
     * on_snapshot after), a timer tick runs on_timer at now(), an order update or a fill is
     * booked, a task runs. The event loop only brings the events here. */
    void process_event(const model::Event& event);
    /** Run fn on the event thread, after the events put before it, and return its result
     * (rethrowing what it throws); in place when called there. The trading state (strategies,
     * holdings, orders) is the event thread's alone: another thread (gRPC) reads or changes it
     * through here. */
    template <typename Fn> auto on_event_thread(Fn&& fn) -> std::invoke_result_t<Fn&> {
        using Result = std::invoke_result_t<Fn&>;
        if (event_engine_->in_event_thread()) {
            return fn();
        }
        // The task owns the work: dropped unrun (the queue full, or the engine stopping), it
        // breaks the promise and get() throws instead of waiting for ever.
        auto work = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
        std::future<Result> done = work->get_future();
        put_event(model::Event(model::EventType::Task,
                                   new model::Task([work = std::move(work)] { (*work)(); })));
        return done.get();
    }

    // ---- Booking: every order update and fill (event thread) ----

    /** An order's update: the execution engine keeps it, the holdings see it, its strategy hears
     * of it; saved when it is a strategy's. An end whose fills are not all booked waits for them
     * (ExecutionEngine::defer_terminal_update). */
    void book_order(model::OrderData& order);
    /** A fill, once (one booked already is dropped: IB repeats today's when the engine
     * reconnects): the execution engine keeps it; a strategy's moves its holding, is saved (the
     * holding after it too), and the strategy hears of it. A fill of an order no strategy sent is
     * held by none. */
    void book_trade(const model::TradeData& trade);

    /** A timer tick: options past their expiry are settled first, so what follows sees what is
     * left; then the strategies, the holdings' metrics and the hedges. */
    void on_timer(model::DateTime now);

    // ---- Orders, through the venue ----

    /** The venue's id for the order; empty when it is rejected. Not a strategy's. */
    std::string send_order(const model::OrderRequest& req);
    /** A strategy's (or its hedge's) order: past the pre-trade check to the venue, and tracked
     * as the strategy's once the venue took it. The venue's id; empty when it is rejected. */
    std::string send_order(const std::string& strategy_name,
                           const model::OrderRequest& req) override;
    /** To the venue; the order is no longer tracked as working. */
    void cancel_order(const model::CancelRequest& req) override;
    model::OrderData* get_order(const std::string& orderid);
    model::TradeData* get_trade(const std::string& tradeid);
    std::vector<model::OrderData> get_all_orders() const;
    std::vector<model::TradeData> get_all_trades() const;
    std::vector<model::OrderData> get_all_active_orders() const;

    // ---- The broker ----

    /** Connect to the venue; with a broker behind it, catch up and check the positions (failures
     * are logged, and keep the strategies from starting). Throws when it cannot be reached. */
    void connect();
    void disconnect();
    bool connected() const;
    /** The account at the broker: its values, and its positions with what each is worth. Throws
     * when it cannot be asked (not connected; no broker) or does not answer in a minute. */
    adapters::BrokerAccount query_account();
    /** The portfolio's underlying and its options expiring within max_dte days, from the
     * broker, saved; the engine trades them from its next start. Blocks until it answers. */
    std::vector<model::ContractData> sync_contracts(const std::string& portfolio, int max_dte);

    // ---- Portfolios and holdings ----

    model::PortfolioData* get_portfolio(const std::string& name);
    const model::ContractData* get_contract(const std::string& symbol) const;
    model::StrategyHolding* get_holding(const std::string& strategy_name);
    const model::StrategyHolding* get_holding(const std::string& strategy_name) const;
    std::vector<std::string> get_all_portfolio_names() const;
    std::vector<model::ContractData> get_all_contracts() const;

    // ---- Strategies. What the operator does to one is kept in the storage, so that a restart
    // brings it back as it was. Each runs on the event thread, in turn with the events.

    /** The strategies as a restart would bring them back: class, portfolio, setting, and the
     * state the operator left them in. */
    std::vector<adapters::StrategyRecord> strategies();
    /** Each strategy's holding as it is now. */
    std::unordered_map<std::string, model::StrategyHolding> holdings();
    /** Each strategy in error, with its message. */
    std::vector<std::pair<std::string, std::string>> get_strategy_errors();
    static std::vector<std::string> strategy_classes();
    /** A class's setting before overrides (OptionStrategyEngine::get_default_setting). */
    std::unordered_map<std::string, double>
    strategy_class_defaults(const std::string& class_name) const;

    /** OptionStrategyEngine::add_strategy (a name is a strategy's for good); returns its name. */
    std::string add_strategy(const std::string& class_name, const std::string& portfolio,
                             const std::unordered_map<std::string, double>& setting,
                             const std::string& strategy_name = {});
    void init_strategy(const std::string& name);
    /** Refused for PositionEngine::start_refusal: with a broker, the positions must have been
     * checked and agree (OPTRA_RECONCILE=warn: not). */
    void start_strategy(const std::string& name);
    void stop_strategy(const std::string& name);
    /** OptionStrategyEngine::remove_strategy; restore_strategy brings it back. */
    void remove_strategy(const std::string& name);
    /** Forget a strategy for good, removing it first; its orders and trades stay, under a name
     * no strategy takes again. */
    void delete_strategy(const std::string& name);
    std::vector<std::string> removed_strategies();
    /** Add a removed strategy back, stopped, its holding rebooked from its trades. */
    void restore_strategy(const std::string& name);

    // ---- Risk: every order passes RiskEngine; the operator's stop ----

    /** Halt trading: every strategy stopped, every working order cancelled, and no order sent
     * but one that only closes, until clear_halt(). Saved: a restart stays halted. */
    void halt(const std::string& reason);
    void clear_halt();
    /** Set risk limits (RiskEngine::set; throws on an unknown one or a value out of range);
     * saved. */
    void set_risk_settings(const std::unordered_map<std::string, double>& values);
    /** Book a fill by hand: an order of the strategy's filled at once, with its fill (reference
     * "manual: reason"), saved and replayed on a restart like any. For a position the engine
     * missed (the positions check disagrees and the broker is right). Returns its id. */
    std::string book_manually(const std::string& strategy_name, const std::string& symbol,
                              model::Direction direction, double volume, double price,
                              const std::string& reason);

    // ---- The storage, as the operator sees it ----

    /** The orders (trades) saved, of one strategy (empty: all), the latest `latest` of them (0:
     * all), in the order saved. */
    std::vector<adapters::SavedRecord> saved_orders(const std::string& strategy, std::size_t latest);
    std::vector<adapters::SavedRecord> saved_trades(const std::string& strategy, std::size_t latest);
    /** Every strategy name orders or trades are saved under. */
    std::vector<std::string> strategies_with_records();
    /** The contracts saved: what the engine trades from its next start. */
    std::vector<model::ContractData> saved_contracts();
    /** Empty "trading" (the orders, trades and holdings) or "contracts"; each table with the rows
     * it lost. The trading record is refused while a strategy holds positions or has orders
     * working: a restart would not know them. Contracts loaded stay until the next start. */
    std::vector<std::pair<std::string, int>> wipe_database(const std::string& what);

    // ---- Positions against the broker, and expiry ----

    /** Ask the broker for the account and check the holdings against its positions, and record
     * it (PositionEngine::record_broker_check). Throws when the broker cannot be asked. */
    std::vector<core::PositionCheck> reconcile_positions();
    /** The same with the account the broker sent. */
    std::vector<core::PositionCheck> reconcile_positions(const adapters::BrokerAccount& account);
    /** Each contract held here or in the account, the holdings against the account's position
     * (an empty account: the holdings alone). Changes nothing. */
    std::vector<core::PositionCheck> check_positions(const adapters::BrokerAccount& account);
    /** PositionEngine::broker_check. */
    std::pair<std::string, std::string> positions_check();
    /** Options held past their close, settled at the underlying's last quote when it is from the
     * close; else they wait (unsettled_options) for settle_expired (event thread). */
    void settle_due(model::DateTime now);
    /** Settle the portfolio's options that expired on `expiry` (a New York date), the underlying
     * at `price` at that close, which is before `now`: the ones settle_due could not (no quote
     * from the close). Returns how many settlements were booked (event thread). */
    int settle_expired(const std::string& portfolio, std::chrono::year_month_day expiry,
                       double price, model::DateTime now);
    /** The same, now, on the event thread. */
    int settle_expired(const std::string& portfolio, std::chrono::year_month_day expiry,
                       double price);
    /** Per strategy, the expired options settle_due last found waiting for a closing price
     * (event thread). */
    const std::unordered_map<std::string, std::vector<std::string>>& unsettled_options() const {
        return unsettled_options_;
    }
    /** unsettled_options, from any thread. */
    std::unordered_map<std::string, std::vector<std::string>> unsettled();

    // ---- To the control plane ----

    bool pop_log_for_stream(utilities::LogData& out, int timeout_ms);

    // ---- The market data (live; a backtest replays its own) ----

    /** Stream these chains for the strategy, in place of what it had; none without the port. */
    void subscribe_chains(const std::string& strategy_name,
                          std::span<const std::string> chains) override;
    void unsubscribe_chains(const std::string& strategy_name) override;
    /** A name some strategy had before (its orders and trades kept under it): never another's. */
    bool is_strategy_name_used(const std::string& strategy_name) override;

    /** Stop the strategies and the ports. */
    void close();

  protected:
    /** Destroyed as the assembly it is. */
    ~Engine();

    /** The core engines, around the ports (set before): the execution engine sends and cancels
     * through the venue, whose order updates and fills come in as events. */
    void init_core(utilities::LogLevel log_level);
    /** The strategies, orders and trades from the storage (event thread). */
    void restore();
    /** Queue a copy of `data` in the pooled `slot` as an event (dropped and logged when the pool
     * is closed: slot is null). */
    template <typename T> void post(model::EventType type, T* slot, const T& data) {
        if (slot == nullptr) {
            model::Event dropped(type);
            event_engine_->drop(dropped, "its pool is closed");
            return;
        }
        *slot = data;
        put_event(model::Event(type, slot));
    }

    // First, so it is there for every engine (they log to it) and outlives them all.
    std::unique_ptr<core::LogEngine> log_engine_;
    std::unique_ptr<model::PortfolioStructure> portfolio_structure_;
    std::unique_ptr<core::ExecutionEngine> execution_engine_;
    std::unique_ptr<core::PositionEngine> position_engine_;
    std::unique_ptr<core::HedgeEngine> hedge_engine_;
    std::unique_ptr<core::RiskEngine> risk_engine_;
    std::unique_ptr<core::OptionStrategyEngine> option_strategy_engine_; // holds the ones above

    // The ports, set by the assembly. Destroyed in reverse: the market data and the venue first
    // (their callbacks reach the event loop), the storage last (the others save to it).
    std::function<model::DateTime()> clock_;
    model::DateTime snapshot_time_{}; // the latest snapshot's: a backtest's clock
    std::unique_ptr<adapters::DatabaseEngine> db_engine_;
    std::unique_ptr<EventEngine> event_engine_;
    std::unique_ptr<adapters::Gateway> gateway_;
    std::unique_ptr<adapters::MarketDataEngine> market_data_; // none in a backtest

    bool reconcile_warn_only_ = false; // OPTRA_RECONCILE=warn


  private:
    /** The ports, closed: the market data first, the storage last. */
    void close_ports();
    /** Apply an order update: merge it into the stored order; when it changed, save it and notify
     * its strategy. */
    void apply_order(const model::OrderData& update);
    /** The holdings changed (a fill booked, a restore): the options held keep streaming. */
    void on_positions_changed();
    void save_order_data(const std::string& strategy_name, const model::OrderData& order);
    void save_trade_data(const std::string& strategy_name, const model::TradeData& trade);
    /** Save the strategy's state as its template has it: running, inited or added (event
     * thread). */
    void save_strategy_state(const std::string& name);
    /** Book these strategies' holdings from the storage again, the strategies not hearing of it:
     * every order and trade saved, in the order saved (event thread). */
    void rebook(const std::unordered_set<std::string>& strategies);
    /** Book a settlement: an order of the strategy's filled at once, with its fills. */
    void book_settlement(const std::string& strategy_name, const model::OrderData& order,
                         std::span<const model::TradeData> fills);
    /** An order of the strategy's filled at once, with its fills: booked as the venue's would be
     * (a settlement, a fill booked by hand). */
    void book_filled_order(const std::string& strategy_name, const model::OrderData& order,
                           std::span<const model::TradeData> fills);
    /** Options settled in shares and expiring today, within flatten_minutes of their close:
     * closed now (RiskEngine::in_flatten_window; a timer tick). */
    void flatten_expiring(model::DateTime now);
    /** The risk limits and the halt as saved (init_core), and saved again. */
    void restore_risk();
    void save_risk();

    std::unordered_map<std::string, std::vector<std::string>> unsettled_options_;
    std::vector<std::string> held_options_;            // the market data was last told to hold
};

} // namespace core
