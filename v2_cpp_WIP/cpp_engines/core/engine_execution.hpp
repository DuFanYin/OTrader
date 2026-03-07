#pragma once

/** ExecutionEngine: order/trade cache, active order tracking. */

#include "../utilities/base_engine.hpp"
#include "../utilities/constant.hpp"
#include "../utilities/object.hpp"
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace core {

class ExecutionEngine : public utilities::BaseEngine {
  public:
    ExecutionEngine() = default;
    explicit ExecutionEngine(utilities::MainEngine* main) : BaseEngine(main, "Execution") {}

    /** Injected send fn; register after send_order. */
    using SendOrderFn = std::function<std::string(const utilities::OrderRequest&)>;
    void set_send_impl(SendOrderFn fn) { send_impl_ = std::move(fn); }

    /** Injected cancel fn; remove_order_tracking after cancel. */
    using CancelImplFn = std::function<void(const utilities::CancelRequest&)>;
    void set_cancel_impl(CancelImplFn fn) { cancel_impl_ = std::move(fn); }

    /** Send order, register strategy↔orderid; pre_trade_risk_check first. */
    std::string send_order(const std::string& strategy_name, const utilities::OrderRequest& req);

    /** Cancel: call impl, remove from tracking. */
    void cancel_order(const utilities::CancelRequest& req);

    /** Account position (placeholder). */
    void set_account_position(const std::string& symbol, double position);
    double get_account_position(const std::string& symbol) const;

    /** Pre-trade risk check (placeholder, pass). */
    static bool pre_trade_risk_check(const std::string& strategy_name,
                                     const utilities::OrderRequest& req);

    /** Register strategy↔orderid (called by strategy after send). */
    void register_active_order(const std::string& strategy_name, const std::string& orderid);

    /** Store order; remove from active if CANCELLED/REJECTED/ALLTRADED. */
    void store_order(const std::string& strategy_name, const utilities::OrderData& order);

    /** Store order only (backtest add_order; no active update). */
    void add_order(const utilities::OrderData& order);

    /** Store trade. */
    void store_trade(const utilities::TradeData& trade);

    utilities::OrderData* get_order(const std::string& orderid);
    utilities::TradeData* get_trade(const std::string& tradeid);
    std::string get_strategy_name_for_order(const std::string& orderid) const;

    std::vector<utilities::OrderData> get_all_orders() const;
    std::vector<utilities::TradeData> get_all_trades() const;
    std::vector<utilities::OrderData> get_all_active_orders() const;

    /** strategy_name → active orderids. */
    const std::unordered_map<std::string, std::set<std::string>>&
    get_strategy_active_orders() const;

    /** Remove orderid from tracking. */
    void remove_order_tracking(const std::string& orderid);

    /** Clear strategy's active orders (remove_strategy). */
    void remove_strategy_tracking(const std::string& strategy_name);

    /** Backtest: insert orderid after append_order. */
    std::unordered_set<std::string>& active_order_ids() { return all_active_order_ids_; }
    const std::unordered_set<std::string>& active_order_ids() const {
        return all_active_order_ids_;
    }

    /** Ensure strategy key (add_strategy). */
    void ensure_strategy_key(const std::string& strategy_name);

    /** Clear all cache on close. */
    void clear();

    void close() override { clear(); }

  private:
    SendOrderFn send_impl_;
    CancelImplFn cancel_impl_;
    std::unordered_map<std::string, double> account_position_;
    std::unordered_map<std::string, utilities::OrderData> orders_;
    std::unordered_map<std::string, utilities::TradeData> trades_;
    std::unordered_map<std::string, std::string> orderid_strategy_name_;
    std::unordered_map<std::string, std::set<std::string>> strategy_active_orders_;
    std::unordered_set<std::string> all_active_order_ids_;
};

} // namespace core
