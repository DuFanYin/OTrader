#pragma once

/** ExecutionEngine: orders, fills, working orders per strategy, and terminal updates deferred until
 * their fills are booked. Engine::send_order registers each order the venue accepts. The result
 * does not depend on the arrival order of order updates and fills. */

#include "utilities/base_engine.hpp"
#include "model/constant.hpp"
#include "model/object.hpp"
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

class ExecutionEngine : public utilities::BaseEngine {
  public:
    ExecutionEngine() = default;
    explicit ExecutionEngine(utilities::Logger* logger) : BaseEngine(logger, "Execution") {}

    /** How long a deferred terminal update waits for its fills (defer_terminal_update). */
    static constexpr std::chrono::seconds kTerminalUpdateTimeout{10};

    /** Register strategy↔orderid, working. */
    void register_active_order(const std::string& strategy_name, const std::string& orderid);
    /** An order the venue accepted (Engine::send_order): stored as sent (contract, legs),
     * registered to the strategy and working, before any update from the venue. */
    void register_sent_order(const std::string& strategy_name, const model::OrderData& order);

    /** Merge a venue's order update into the stored order. A terminal status is final; traded
     * only increases; the legs are the ones sent. A terminal order is removed from the working
     * orders. Returns the stored order, or null when the update changes nothing (a repeated or
     * out-of-date update). */
    const model::OrderData* process_order_event(const std::string& strategy_name,
                                                const model::OrderData& order);

    /** Store an order as it is (restore_order). */
    void add_order(const model::OrderData& order);

    /** Store a fill by tradeid, and add its volume to its order's filled volume while the order is
     * working or has a deferred terminal update. */
    void process_trade_event(const model::TradeData& trade);

    /** Defer a terminal update of a strategy's order whose fills are not all booked (a venue may
     * send the terminal status before the fills; IB does for a combo's legs). It is returned by
     * take_ready_terminal_update once the fills are booked, or by take_expired_terminal_updates
     * after kTerminalUpdateTimeout. Returns true when deferred. */
    bool defer_terminal_update(const model::OrderData& update, model::DateTime now);
    /** The deferred terminal update of the fill's order, if its fills are now all booked. */
    std::optional<model::OrderData> take_ready_terminal_update(const model::TradeData& trade);
    /** The deferred terminal updates older than kTerminalUpdateTimeout, sorted by orderid. */
    std::vector<model::OrderData> take_expired_terminal_updates(model::DateTime now);

    model::OrderData* get_order(const std::string& orderid);
    const model::OrderData* get_order(const std::string& orderid) const;
    model::TradeData* get_trade(const std::string& tradeid);
    std::string get_strategy_name_for_order(const std::string& orderid) const;

    std::vector<model::OrderData> get_all_orders() const;
    std::vector<model::TradeData> get_all_trades() const;
    std::vector<model::OrderData> get_all_active_orders() const;

    /** The strategy's orders working (the venue took them, not finished yet). */
    const std::set<std::string>& active_orders(const std::string& strategy_name) const;
    /** A strategy's order the venue took, not finished yet. */
    bool is_working(const std::string& orderid) const;

    /** The order is no longer working (its strategy stays known). */
    void remove_order_tracking(const std::string& orderid);
    /** An order from before a restart: kept as it was, owned by strategy_name, and working again
     * if it was. */
    void restore_order(const std::string& strategy_name, const model::OrderData& order);

    /** Forget the strategy's orders and their fills (remove_strategy): restored, it books them
     * again. */
    void remove_strategy_tracking(const std::string& strategy_name);

    /** Clear all cache on close. */
    void clear();

    void close() { clear(); }

  private:
    /** Booked fills cover the update's traded volume: for a combo, each leg's traded times its
     * ratio (legs as sent). */
    [[nodiscard]] bool fills_cover_traded(const model::OrderData& update) const;

    struct DeferredUpdate {
        model::OrderData update;
        model::DateTime since;
    };

    std::unordered_map<std::string, model::OrderData> orders_;
    std::unordered_map<std::string, model::TradeData> trades_;
    /** orderid → contract → booked volume, for working orders and deferred terminal updates. */
    std::unordered_map<std::string, std::unordered_map<std::string, double>> filled_volume_;
    std::unordered_map<std::string, DeferredUpdate> deferred_updates_;
    std::unordered_map<std::string, std::string> orderid_strategy_name_;
    std::unordered_map<std::string, std::set<std::string>> strategy_active_orders_;
};

} // namespace core
