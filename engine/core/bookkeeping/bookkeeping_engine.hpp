#pragma once

/**
 * BookkeepingEngine: the record of each strategy's orders, their fills and the holding the fills
 * make; the holding's value, its settlement at expiry and its check against the broker's account.
 * A fill is booked once, to the strategy whose order it is, and an order's end is held back until
 * its fills are booked. Active is one thing: a strategy's order the venue has not ended (a cancel
 * sent does not end it). The result is independent of the order updates and fills arrive in.
 * State only, event thread.
 */

#include "utilities/log_source.hpp"
#include "model/constant.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace model {
class Portfolios;
} // namespace model

namespace core {

/** Engine vs broker quantity of one contract (options in contracts). */
struct BrokerCheckRow {
    std::string symbol;
    double engine = 0.0;
    double broker = 0.0;
    /** Must agree when true; else broker-only (traded by hand, say), listed apart. */
    bool traded_here = false;

    [[nodiscard]] bool is_mismatch() const;
};

class BookkeepingEngine {
  public:
    /** How long a strategy's order end waits for its fills before it is told anyway. */
    static constexpr std::chrono::seconds kTerminalUpdateTimeout{10};

    /** An instantly filled order booking a close-out or share delivery. */
    struct Settlement {
        model::Order order;
        std::vector<model::Fill> fills;
    };

    BookkeepingEngine() = default;
    explicit BookkeepingEngine(utilities::Logger* logger) : log_(logger, "Bookkeeping") {}

    // ---- Orders and fills

    /** The strategy's order as sent (legs included, active before any venue update arrives), or
     * as saved before a restart (active if it was). */
    void add_order(const std::string& strategy_name, model::Order order);
    /** A venue's update merged into the order (an end is final, filled only increases, the legs
     * stay as sent): the order to tell its strategy of now. None when it changes nothing, or when
     * it ends a strategy's order whose fills are not all booked yet: held back until they are
     * (take_released) or kTerminalUpdateTimeout passes (take_overdue). */
    std::optional<model::Order> update_order(const model::Order& update,
                                             model::DateTime now);
    /** Books the fill to the holding of the strategy whose order it is, whatever the order was;
     * the order only names the combo a position opened from flat was opened in. The strategy's
     * name (empty: no strategy's order, held by none); null when booked already (a venue repeats
     * fills). */
    const std::string* book_fill(const model::Fill& fill);
    /** The end of the fill's order, merged, once its fills are all booked. */
    std::optional<model::Order> take_released(const model::Fill& fill);
    /** The ends held back kTerminalUpdateTimeout or longer, merged, by order_id. */
    std::vector<model::Order> take_overdue(model::DateTime now);
    /** A cancel goes to the venue: false when the order is not active or one went already. The
     * order works on until the venue ends it. */
    bool cancel_sent(const std::string& order_id);

    const model::Order* find_order(const std::string& order_id) const;
    /** Valid until the next fill is booked. */
    const model::Fill* find_fill(const std::string& fill_id) const;
    /** Empty when no strategy's. */
    const std::string& strategy_of(const std::string& order_id) const;
    bool is_active(const std::string& order_id) const;
    bool has_active_order(const std::string& strategy_name) const;
    /** f(order) for each active order of the strategy, or of every strategy when strategy_name is
     * empty (an order no strategy sent is never active here), by order_id. */
    template <typename F>
    void for_each_active_order(const std::string& strategy_name, F&& f) const {
        if (!strategy_name.empty()) {
            if (const auto it = books_.find(strategy_name); it != books_.end()) {
                for (const auto& [_, t] : it->second.active) {
                    f(t->order);
                }
            }
            return;
        }
        std::vector<const Tracked*> all;
        for (const auto& [_, books] : books_) {
            for (const auto& [__, t] : books.active) {
                all.push_back(t);
            }
        }
        std::ranges::sort(all, {}, [](const Tracked* t) -> const std::string& {
            return t->order.order_id;
        });
        for (const Tracked* t : all) {
            f(t->order);
        }
    }
    /** f(contract, quantity) for what the strategy's active orders would still trade, each
     * contract once: signed (bought positive), every leg's ratio times its order's remaining. */
    template <typename F> void for_each_working(const std::string& strategy_name, F&& f) const {
        if (const auto it = books_.find(strategy_name); it != books_.end()) {
            for (const auto& [symbol, quantity] : it->second.working) {
                f(symbol, quantity);
            }
        }
    }
    [[nodiscard]] std::size_t order_count() const { return orders_.size(); }
    /** In the order booked. */
    [[nodiscard]] std::span<const model::Fill> fills() const { return fills_; }
    /** Forgets the orders done with (ended, no end held back) and their fills. For a backtest of
     * many days, each day's taken: kept, they would grow with the days. */
    void forget_finished();

    // ---- Holdings

    model::StrategyHolding& ensure_holding(const std::string& strategy_name);
    const model::StrategyHolding* find_holding(const std::string& strategy_name) const;
    /** f(name, holding) for each strategy's holding. */
    template <typename F> void for_each_holding(F&& f) const {
        for (const auto& [name, books] : books_) {
            f(name, books.holding);
        }
    }
    bool holds(const std::string& strategy_name, const std::string& symbol) const;
    /** By every strategy, sorted. */
    std::vector<std::string> held_options() const;
    /** Whether held_options() may have changed since the last call (a position opened or closed,
     * a strategy removed). */
    bool take_held_options_changed();
    /** Each timer tick: the holding valued at the portfolio's quotes, its P&L attributed. */
    void value_holding(const std::string& strategy_name, const model::Portfolio& portfolio);

    /** Forgets the strategy's orders, fills and holding; restoring it books them again. */
    void remove_strategy(const std::string& strategy_name);
    void close();

    // ---- Expiry

    /** Cash-settled options close at intrinsic; share-settled ones close at zero and, if ITM by a
     * cent or more (OCC exercise by exception), deliver shares at the strike. Closes first, then
     * deliveries; reference "Settlement". */
    std::vector<Settlement> settle_expiry(const std::string& strategy_name,
                                          std::chrono::year_month_day expiry, double price,
                                          const model::Portfolio& portfolio) const;
    /** The strategy's expired options that settle now, by expiry with its closing price. The rest
     * wait in unsettled_options (logged when that changes). Each timer tick. */
    std::vector<std::pair<std::chrono::year_month_day, double>>
    expiries_to_settle(const std::string& strategy_name, const model::Portfolio* portfolio,
                       model::DateTime now);
    /** Strategy → expired options without a closing price, for settle_expired by hand. */
    std::unordered_map<std::string, std::vector<std::string>> unsettled_options() const;

    // ---- The broker's account

    /** broker: contract -> signed quantity. Sorted. */
    std::vector<BrokerCheckRow> compare_with_broker(const std::map<std::string, double>& broker,
                                                    const model::Portfolios& portfolios) const;
    /** Logs when the outcome changed. */
    void record_broker_check(std::span<const BrokerCheckRow> checks);
    void record_broker_check_failed(const std::string& reason);
    /** "not yet", "agree", "disagree" or "failed", and the problem. */
    std::pair<std::string, std::string> broker_check() const;
    /** Refused for unsettled expired options, or (if required) a broker check that did not
     * agree. */
    std::optional<std::string> start_refusal(const std::string& strategy_name,
                                             model::DateTime now,
                                             bool broker_agreement_required) const;

  private:
    /** An order and what the book keeps of it. */
    struct Tracked {
        model::Order order;
        std::string strategy_name;
        /** Contract -> quantity booked, against what the venue says was filled. */
        std::unordered_map<std::string, double> booked;
        /** An end that came before all its fills, and since when. */
        std::optional<model::Order> held_end;
        model::DateTime held_since{};
        bool cancel_sent = false;
        /** The remaining its strategy's working counts for it: 0 once it ends. */
        double working = 0.0;
    };

    /** The update merged into t: false when it changes nothing. */
    static bool merge(Tracked& t, const model::Order& update);
    /** Each leg's booked quantity covers `filled` units by its ratio. */
    static bool fills_cover(const Tracked& t, double filled);
    /** Active: in its strategy's active, and its remaining in its working, while the order has
     * not ended. */
    void track_active(const std::string& order_id, Tracked& t);

    /** The fill into the strategy's holding; `order` (null when unknown) names the combo. */
    void book_to_holding(const std::string& strategy_name, const model::Fill& fill,
                         const model::Order* order);

    /** The holding at its last update: the baseline for P&L attribution. */
    struct AttributionStart {
        model::DateTime time{};
        double pnl = 0.0;
        double underlying_price = 0.0;
        model::Position shares;
        std::vector<model::Position> options; // mid, Greeks per contract
        std::vector<double> option_ivs;
    };
    /** A strategy's books: its holding, its attribution's baseline, the expired options it holds
     * without a closing price to settle at, and its orders the venue has not ended (by order_id;
     * orders_'s nodes stay put) with what they would still trade, by contract. */
    struct Books {
        model::StrategyHolding holding;
        AttributionStart attribution;
        std::vector<std::string> unsettled;
        std::map<std::string, const Tracked*> active;
        std::unordered_map<std::string, double> working;
    };
    /** Adds moves since the last update to pnl_*, then resets the baseline. */
    static void attribute_pnl(Books& books, const model::Portfolio& portfolio);
    /** units of the order's legs (each by its side and ratio) into the books' working. */
    static void add_working(Books& books, const model::Order& order, double units);
    /** The fills of these orders forgotten. */
    void forget_fills(const std::unordered_set<std::string>& order_ids);

    enum class BrokerCheck : std::uint8_t { NotYet, Agree, Disagree, Failed };

    utilities::LogSource log_;
    std::unordered_map<std::string, Tracked> orders_;
    std::vector<model::Fill> fills_;                          // in the order booked
    std::unordered_map<std::string, std::size_t> fill_index_; // by fill_id: into fills_
    std::unordered_map<std::string, Books> books_; // by strategy
    bool held_options_changed_ = false;
    BrokerCheck broker_check_ = BrokerCheck::NotYet;
    std::string broker_check_problem_;
};

} // namespace core
