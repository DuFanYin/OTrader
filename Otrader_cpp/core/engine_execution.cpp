#include "engine_execution.hpp"
#include "../utilities/utility.hpp"

namespace core {

auto ExecutionEngine::send_order(const std::string& strategy_name,
                                 const utilities::OrderRequest& req) -> std::string {
    if (!pre_trade_risk_check(strategy_name, req)) {
        return {};
    }
    std::string orderid = send_impl_ ? send_impl_(req) : std::string{};
    if (!orderid.empty()) {
        register_active_order(strategy_name, orderid);
    }
    return orderid;
}

void ExecutionEngine::set_account_position(const std::string& symbol, double position) {
    // placeholder: 后续由 runtime 注入或从 gateway 同步
    account_position_[symbol] = position;
}

auto ExecutionEngine::get_account_position(const std::string& symbol) const -> double {
    (void)symbol;
    auto it = account_position_.find(symbol);
    return (it != account_position_.end()) ? it->second : 0.0;
}

auto ExecutionEngine::pre_trade_risk_check(const std::string& strategy_name,
                                           const utilities::OrderRequest& req) -> bool {
    (void)strategy_name;
    (void)req;
    // placeholder: 后续实现额度、持仓、频率等检查
    return true;
}

void ExecutionEngine::register_active_order(const std::string& strategy_name,
                                            const std::string& orderid) {
    if (orderid.empty()) {
        return;
    }
    strategy_active_orders_[strategy_name].insert(orderid);
    orderid_strategy_name_[orderid] = strategy_name;
    all_active_order_ids_.insert(orderid);
}

void ExecutionEngine::store_order(const std::string& strategy_name,
                                  const utilities::OrderData& order) {
    orders_[order.orderid] = order;
    if (order.status == utilities::Status::CANCELLED ||
        order.status == utilities::Status::REJECTED ||
        order.status == utilities::Status::ALLTRADED) {
        strategy_active_orders_[strategy_name].erase(order.orderid);
        orderid_strategy_name_.erase(order.orderid);
        all_active_order_ids_.erase(order.orderid);
    }
}

void ExecutionEngine::add_order(const utilities::OrderData& order) {
    orders_[order.orderid] = order;
}

void ExecutionEngine::store_trade(const utilities::TradeData& trade) {
    trades_[trade.tradeid] = trade;
}

auto ExecutionEngine::get_order(const std::string& orderid) -> utilities::OrderData* {
    auto it = orders_.find(orderid);
    return (it != orders_.end()) ? &it->second : nullptr;
}

auto ExecutionEngine::get_trade(const std::string& tradeid) -> utilities::TradeData* {
    auto it = trades_.find(tradeid);
    return (it != trades_.end()) ? &it->second : nullptr;
}

auto ExecutionEngine::get_strategy_name_for_order(const std::string& orderid) const -> std::string {
    auto it = orderid_strategy_name_.find(orderid);
    return (it != orderid_strategy_name_.end()) ? it->second : std::string{};
}

auto ExecutionEngine::get_all_orders() const -> std::vector<utilities::OrderData> {
    std::vector<utilities::OrderData> out;
    out.reserve(orders_.size());
    for (const auto& [_, o] : orders_) {
        out.push_back(o);
    }
    return out;
}

auto ExecutionEngine::get_all_trades() const -> std::vector<utilities::TradeData> {
    std::vector<utilities::TradeData> out;
    out.reserve(trades_.size());
    for (const auto& [_, t] : trades_) {
        out.push_back(t);
    }
    return out;
}

auto ExecutionEngine::get_all_active_orders() const -> std::vector<utilities::OrderData> {
    std::vector<utilities::OrderData> out;
    for (const std::string& oid : all_active_order_ids_) {
        auto it = orders_.find(oid);
        if (it != orders_.end() && it->second.is_active()) {
            out.push_back(it->second);
        }
    }
    return out;
}

auto ExecutionEngine::get_strategy_active_orders() const
    -> const std::unordered_map<std::string, std::set<std::string>>& {
    return strategy_active_orders_;
}

void ExecutionEngine::remove_order_tracking(const std::string& orderid) {
    auto it = orderid_strategy_name_.find(orderid);
    if (it != orderid_strategy_name_.end()) {
        strategy_active_orders_[it->second].erase(orderid);
        orderid_strategy_name_.erase(it);
    }
    all_active_order_ids_.erase(orderid);
}

void ExecutionEngine::remove_strategy_tracking(const std::string& strategy_name) {
    auto oit = strategy_active_orders_.find(strategy_name);
    if (oit != strategy_active_orders_.end()) {
        for (const std::string& oid : oit->second) {
            orderid_strategy_name_.erase(oid);
            all_active_order_ids_.erase(oid);
        }
        strategy_active_orders_.erase(oit);
    }
}

void ExecutionEngine::ensure_strategy_key(const std::string& strategy_name) {
    strategy_active_orders_[strategy_name];
}

void ExecutionEngine::clear() {
    strategy_active_orders_.clear();
    orderid_strategy_name_.clear();
    all_active_order_ids_.clear();
    account_position_.clear();
    orders_.clear();
    trades_.clear();
}

} // namespace core
