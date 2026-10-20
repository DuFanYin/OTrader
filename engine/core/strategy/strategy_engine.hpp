#pragma once

/** StrategyEngine: hosts the strategies (backtest and live). It creates each with a context bound
 * to it, keeps its state (added, inited, running, in error), drives its callbacks, each timed and
 * kept from throwing past it, hedges its delta when it asks, and keeps what they want streamed
 * (wanted: what live market data plans and a backtest's day loads). State only, event thread. */

#include "model/daily_table.hpp"
#include "model/object.hpp"
#include "model/portfolios.hpp"
#include "model/utility.hpp"
#include "utilities/latency.hpp"
#include "utilities/log_source.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace strategy {
class Strategy;
}

namespace core {

class BookkeepingEngine;

class StrategyEngine {
  public:
    /** What its strategies' orders and states go to (Main). */
    struct Callbacks {
        /** An order of the strategy's (its own, or its hedge): risk-checked, then to the venue; its
         * id, empty when not sent. */
        std::function<std::string(const model::StrategyRecord& strategy,
                                  const model::OrderRequest& request)>
            send_order;
        /** A cancel of a strategy's hedge, to the venue. */
        std::function<void(const model::CancelRequest& request)> cancel_order;
        /** A strategy's state changed (added, restored and removed included): kept, so a restart
         * finds it as it was. */
        std::function<void(const model::StrategyRecord& record)> state_changed;
    };

    /** How a strategy's delta is hedged (Strategy::register_hedging). */
    struct Hedging {
        int timer_trigger = 5; // every this many bars
        int delta_target = 0;
        int delta_range = 0;
    };

    /** What the strategies want streamed, as one immutable set: each change publishes another,
     * so any thread reads it without a lock. */
    struct Wanted {
        std::vector<std::string> subscribed; // the chains strategies subscribe; sorted, each once
        std::vector<std::string> options;    // held, and ordered today; sorted, each once
        std::vector<std::string> chains;     // subscribed, and the chains of those options
    };

    StrategyEngine(utilities::Logger* logger, const model::Portfolios& portfolios,
                   BookkeepingEngine& book, Callbacks callbacks);
    ~StrategyEngine();
    StrategyEngine(const StrategyEngine&) = delete;
    StrategyEngine& operator=(const StrategyEngine&) = delete;

    // ---- Hosting

    /** The strategy asked for (its class, portfolio and setting, the defaults overridden; its state
     * ignored) hosted, added: returns its name, the record's, or <class>_<portfolio> numbered `_2`,
     * ... when it has none. Names are never reused: throws for one in use or used_before, an
     * unknown class or portfolio, or a rejected setting. */
    std::string add_strategy(model::StrategyRecord asked,
                             const std::function<bool(const std::string&)>& used_before = {});
    /** One saved (after a restart, or a removed one restored) hosted again under its name, added. */
    void restore_strategy(const model::StrategyRecord& record);
    /** False when there is none. Throws while it holds positions or has working orders. Stopped
     * first if running, then removed (its state told); its book, chains and hedging go with it. */
    bool remove_strategy(const std::string& strategy_name);
    /** Dropped, not stopped: on a restart they resume from storage. */
    void close();

    // ---- Its states. Each throws for a strategy that is not there or a step its state does not
    // allow; every change of state goes to Callbacks::state_changed.

    /** Added or in error: inited (its on_init, then the day's on_day); a throw puts it in error. */
    void init_strategy(const std::string& strategy_name);
    /** Inited: running. */
    void start_strategy(const std::string& strategy_name);
    /** Running: inited again, then its on_stop; a throw puts it in error, not passed on. */
    void stop_strategy(const std::string& strategy_name);
    /** In error until inited again; the first cause stays. */
    void set_error(const std::string& strategy_name, const std::string& msg);
    /** Only a strategy `approved` takes (its class and resolved setting; a live account's list)
     * is added or restored from now on; the others are refused. Unset: any. */
    void approve_only(std::function<bool(const model::StrategyRecord& record)> approved) {
        approved_ = std::move(approved);
    }
    /** The values by day its strategies read; it must outlive them. nullptr: none. */
    void use_daily(const model::DailyTable* daily) { daily_ = daily != nullptr ? daily : &kNoDaily; }
    /** Made again from its class and setting, as a restart makes it: its members start over, its
     * holding and orders stay; inited or running, its on_init and the day's on_day again (a throw
     * puts it in error, not passed on). */
    void make_again(const std::string& strategy_name);

    // ---- What it hosts

    /** In the order added: name, class, portfolio, setting (resolved) and state. */
    auto strategies() const {
        return std::views::transform(hosted_, [](const std::unique_ptr<Hosted>& h) -> const auto& {
            return h->record;
        });
    }
    /** The same, each with the portfolio it trades. */
    auto strategies_and_portfolios() const {
        return std::views::transform(hosted_, [](const std::unique_ptr<Hosted>& h) {
            return std::pair<const model::StrategyRecord&, const model::Portfolio&>{h->record,
                                                                              *h->portfolio};
        });
    }
    /** None when there is no such strategy. */
    const model::StrategyRecord* find_record(const std::string& strategy_name) const;
    /** Why the strategy is in error; empty when it is not, or there is no such strategy. */
    std::string error(const std::string& strategy_name) const;
    /** Each strategy in error, and why. */
    std::vector<std::pair<std::string, std::string>> errors() const;
    /** The values it wrote since they were taken last (Strategy::write_value), by name, taken:
     * a backtest's day takes them at its close. Empty when there is no such strategy. */
    std::map<std::string, double, std::less<>> take_values(const std::string& strategy_name);
    /** The strategy itself (tests, benchmarks); none when there is no such strategy. */
    strategy::Strategy* find_strategy(const std::string& strategy_name) const;

    // ---- What they want streamed

    /** Any thread. */
    [[nodiscard]] std::shared_ptr<const Wanted> wanted() const {
        return wanted_.load(std::memory_order_acquire);
    }
    /** How many times wanted() changed: a new count, wanted() again. */
    [[nodiscard]] std::uint64_t wanted_changes() const { return wanted_changes_; }
    /** The strategy's chains in place of those before; none when there is no such strategy. */
    void subscribe_chains(const std::string& strategy_name, std::span<const std::string> chains);
    /** Its options stream for the rest of the day: the fill and the holding read them. */
    void note_ordered(const model::OrderRequest& request);
    /** At a contract roll: the day before's orders are done. */
    void clear_ordered();
    /** The options held, in place of those before. */
    void set_held_options(std::span<const std::string> options);

    // ---- Its hedging

    /** None when the strategy does not hedge. */
    const Hedging* hedging(const std::string& strategy_name) const;
    /** Each strategy that hedges, whatever its state (it still holds what it holds), every its
     * timer_trigger bars, once its holding is valued: its active hedges cancelled, or else an order
     * of whole units of the hedge instrument back to delta_target (none while within
     * delta_range). Hedges are never stacked. */
    void run_hedging();

    // ---- Events, each to the strategies it is for

    void on_order(const model::Order& order);
    void on_fill(const model::Fill& fill);
    /** on_timer for the running strategies, every timer_trigger bars of each. */
    void on_timer();
    /** on_snapshot for the portfolio's running strategies. */
    void on_snapshot(std::string_view portfolio_name);
    /** After a contract roll: on_day for the portfolio's inited and running strategies, once a
     * trading day. */
    void on_day(const std::string& portfolio_name);

    // ---- How long callbacks take

    /** Callbacks over budget warn (at most once a minute per strategy); kCallbacksOverToStop in a
     * row put the strategy in error, positions left for the operator. Zero (backtest): timed only,
     * so machine load cannot change a backtest's result. */
    void set_callback_budget(std::chrono::microseconds budget) { callback_budget_ = budget; }
    static constexpr std::chrono::microseconds kLiveCallbackBudget{5000};
    static constexpr int kCallbacksOverToStop = 10;
    /** One indented line per strategy. */
    [[nodiscard]] std::string callback_report() const;

  private:
    class Context;
    /** A strategy, its context and state, and how long its callbacks take (held by pointer: its
     * context points at it, and the histogram cannot move). */
    struct Hosted {
        model::StrategyRecord record;
        std::string error; // why it is in error
        const model::Portfolio* portfolio = nullptr;
        std::unique_ptr<Context> context;
        std::unique_ptr<strategy::Strategy> strategy;
        std::chrono::year_month_day day{}; // the trading day its on_day last ran for
        std::vector<std::string> chains;   // it subscribes
        std::map<std::string, double, std::less<>> values; // written since taken (write_value)
        model::BarGate timer;              // its on_timer, every timer_trigger bars
        std::optional<Hedging> hedging;
        model::BarGate hedge_timer;
        utilities::LatencyHistogram took;
        int over_in_a_row = 0;
        std::chrono::steady_clock::time_point warned{};
    };

    /** Hosted, added: its setting resolved, its class approved. */
    void create_strategy(model::StrategyRecord record);
    /** None when there is no such strategy. */
    Hosted* hosted(const std::string& strategy_name) const;
    /** Throws when there is no such strategy. */
    Hosted& hosted_named(const std::string& strategy_name) const;
    void enter(Hosted& h, model::StrategyState state);
    void set_error(Hosted& h, const std::string& msg);
    /** Its on_day, once a trading day, while inited or running. */
    void day(Hosted& h);
    void write_log(const Hosted& h, const std::string& msg, utilities::LogLevel level) const;
    void register_hedging(Hosted& h, const Hedging& hedging);
    /** The order back to delta_target, or none. */
    std::optional<model::OrderRequest> hedge_order(const Hosted& h,
                                                   const model::StrategyHolding& holding) const;
    /** h's callback, timed; a throw puts the strategy in error. See set_callback_budget. */
    template <typename F> void timed(Hosted& h, std::string_view callback, F&& f);
    /** Its chains in place of those before. */
    void subscribe(Hosted& h, std::span<const std::string> chains);
    /** Wanted made again from the strategies' chains and the options, and published. */
    void wanted_changed();

    utilities::Logger* logger_; // the strategies' own lines, under their names
    utilities::LogSource log_;  // the engine's
    const model::Portfolios& portfolios_;
    BookkeepingEngine& bookkeeping_;
    static inline const model::DailyTable kNoDaily{};
    const model::DailyTable* daily_ = &kNoDaily; // not owned
    std::function<bool(const model::StrategyRecord& record)> approved_; // approve_only's; empty: any
    Callbacks callbacks_;
    std::vector<std::unique_ptr<Hosted>> hosted_;
    std::chrono::microseconds callback_budget_{0}; // zero: timed only
    std::vector<std::string> held_options_;
    std::unordered_set<std::string> ordered_options_;
    std::atomic<std::shared_ptr<const Wanted>> wanted_{std::make_shared<const Wanted>()};
    std::uint64_t wanted_changes_ = 0;
};

} // namespace core
