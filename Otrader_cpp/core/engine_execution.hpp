#pragma once

/**
 * ExecutionEngine (core): order/trade 缓存与活跃订单跟踪。
 * 从 MainEngine 的 get_order/get_trade 与 OptionStrategyEngine 的 orders_/trades_ 容器抽离，
 * 供 backtest 与 live 共用；策略引擎与 main 通过本引擎存取订单/成交。
 */

#include "../utilities/constant.hpp"
#include "../utilities/object.hpp"
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace core {

class ExecutionEngine {
  public:
    ExecutionEngine() = default;

    /** 由 runtime（Main）注入实际发单函数；send_order 内部调用后 register。 */
    using SendOrderFn = std::function<std::string(const utilities::OrderRequest&)>;
    void set_send_impl(SendOrderFn fn) { send_impl_ = std::move(fn); }

    /** 发单并登记 strategy_name ↔ orderid。发单前会做 pre_trade_risk_check。 */
    std::string send_order(const std::string& strategy_name, const utilities::OrderRequest& req);

    /** 账户级持仓（占位，空实现）。由 runtime 注入或后续实现。 */
    void set_account_position(const std::string& symbol, double position);
    double get_account_position(const std::string& symbol) const;

    /** 发单前风控检查（占位，空实现，默认通过）。 */
    static bool pre_trade_risk_check(const std::string& strategy_name,
                                     const utilities::OrderRequest& req);

    /** 下单成功后由策略引擎调用，登记 strategy_name ↔ orderid。 */
    void register_active_order(const std::string& strategy_name, const std::string& orderid);

    /** 存订单；若状态为 CANCELLED/REJECTED/ALLTRADED 则从活跃集合移除。 */
    void store_order(const std::string& strategy_name, const utilities::OrderData& order);

    /** 仅存订单（供 backtest add_order 注入，不更新活跃集合）。 */
    void add_order(const utilities::OrderData& order);

    /** 存成交。 */
    void store_trade(const utilities::TradeData& trade);

    utilities::OrderData* get_order(const std::string& orderid);
    utilities::TradeData* get_trade(const std::string& tradeid);
    std::string get_strategy_name_for_order(const std::string& orderid) const;

    std::vector<utilities::OrderData> get_all_orders() const;
    std::vector<utilities::TradeData> get_all_trades() const;
    std::vector<utilities::OrderData> get_all_active_orders() const;

    /** 按策略名 → 活跃 orderid 集合（供 hedge 等遍历）。 */
    const std::unordered_map<std::string, std::set<std::string>>&
    get_strategy_active_orders() const;

    /** 取消时由 main 调用，从跟踪中移除 orderid。 */
    void remove_order_tracking(const std::string& orderid);

    /** 移除策略时清理该策略的活跃订单跟踪（策略引擎 remove_strategy 调用）。 */
    void remove_strategy_tracking(const std::string& strategy_name);

    /** 供 backtest MainEngine 在 append_order 后插入 orderid。 */
    std::unordered_set<std::string>& active_order_ids() { return all_active_order_ids_; }
    const std::unordered_set<std::string>& active_order_ids() const {
        return all_active_order_ids_;
    }

    /** 确保某策略在 strategy_active_orders 中有条目（add_strategy 时调用）。 */
    void ensure_strategy_key(const std::string& strategy_name);

    /** 关闭时清空所有缓存。 */
    void clear();

  private:
    SendOrderFn send_impl_;
    std::unordered_map<std::string, double> account_position_; // symbol -> position (placeholder)
    std::unordered_map<std::string, utilities::OrderData> orders_;
    std::unordered_map<std::string, utilities::TradeData> trades_;
    std::unordered_map<std::string, std::string> orderid_strategy_name_;
    std::unordered_map<std::string, std::set<std::string>> strategy_active_orders_;
    std::unordered_set<std::string> all_active_order_ids_;
};

} // namespace core
