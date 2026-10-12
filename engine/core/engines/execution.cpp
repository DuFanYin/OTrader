#include "core/engines/execution.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <iterator>
#include <ranges>

namespace core {

void ExecutionEngine::register_active_order(const std::string& strategy_name,
                                            const std::string& orderid) {
    if (orderid.empty()) {
        return;
    }
    strategy_active_orders_[strategy_name].insert(orderid);
    orderid_strategy_name_[orderid] = strategy_name;
}

void ExecutionEngine::register_sent_order(const std::string& strategy_name,
                                     const model::OrderData& order) {
    register_active_order(strategy_name, order.orderid);
    add_order(order);
}

auto ExecutionEngine::process_order_event(const std::string& strategy_name,
                                          const model::OrderData& order)
    -> const model::OrderData* {
    auto [it, added] = orders_.try_emplace(order.orderid, order);
    model::OrderData& known = it->second;
    if (!added) {
        const double traded = std::max(known.traded, order.traded);
        if (!known.is_active()) {
            if (traded == known.traded) {
                return nullptr; // terminal already: repeated or out of date
            }
            known.traded = traded;
        } else {
            if (order.status == known.status && traded == known.traded) {
                return nullptr; // unchanged
            }
            known.status = order.status;
            known.traded = traded;
            if (order.datetime != model::DateTime{}) {
                known.datetime = order.datetime;
            }
        }
    }
    if (!known.is_active()) {
        if (const auto active = strategy_active_orders_.find(strategy_name);
            active != strategy_active_orders_.end()) {
            active->second.erase(order.orderid);
        }
        deferred_updates_.erase(order.orderid);
        filled_volume_.erase(order.orderid);
    }
    return &known;
}

void ExecutionEngine::add_order(const model::OrderData& order) {
    orders_.insert_or_assign(order.orderid, order);
}

void ExecutionEngine::process_trade_event(const model::TradeData& trade) {
    trades_.insert_or_assign(trade.tradeid, trade);
    if (const model::OrderData* order = get_order(trade.orderid);
        deferred_updates_.contains(trade.orderid) || (order != nullptr && order->is_active())) {
        filled_volume_[trade.orderid][trade.symbol] += trade.volume;
    }
}

auto ExecutionEngine::fills_cover_traded(const model::OrderData& update) const -> bool {
    constexpr double kTolerance = 1e-9;
    const model::OrderData* sent = get_order(update.orderid);
    const model::OrderData& order = sent != nullptr ? *sent : update;
    const auto it = filled_volume_.find(update.orderid);
    const auto booked = [&](const std::string& symbol) -> double {
        if (it == filled_volume_.end()) {
            return 0.0;
        }
        const auto f = it->second.find(symbol);
        return f != it->second.end() ? f->second : 0.0;
    };
    if (!order.is_combo || order.legs.empty()) {
        return booked(order.symbol) + kTolerance >= update.traded;
    }
    return std::ranges::all_of(order.legs, [&](const model::Leg& leg) {
        return booked(leg.symbol) + kTolerance >= update.traded * std::max(1, std::abs(leg.ratio));
    });
}

auto ExecutionEngine::defer_terminal_update(const model::OrderData& update, model::DateTime now)
    -> bool {
    if (update.is_active() || update.traded <= 0.0 ||
        get_strategy_name_for_order(update.orderid).empty() || fills_cover_traded(update)) {
        return false;
    }
    if (const auto [it, added] = deferred_updates_.try_emplace(update.orderid, update, now);
        !added) {
        it->second.update = update; // a repeated terminal update replaces the deferred one
    }
    return true;
}

auto ExecutionEngine::take_ready_terminal_update(const model::TradeData& trade)
    -> std::optional<model::OrderData> {
    const auto it = deferred_updates_.find(trade.orderid);
    if (it == deferred_updates_.end() || !fills_cover_traded(it->second.update)) {
        return std::nullopt;
    }
    model::OrderData update = std::move(it->second.update);
    deferred_updates_.erase(it);
    return update;
}

auto ExecutionEngine::take_expired_terminal_updates(model::DateTime now)
    -> std::vector<model::OrderData> {
    std::vector<model::OrderData> out;
    for (auto it = deferred_updates_.begin(); it != deferred_updates_.end();) {
        if (now - it->second.since >= kTerminalUpdateTimeout) {
            out.push_back(std::move(it->second.update));
            it = deferred_updates_.erase(it);
        } else {
            ++it;
        }
    }
    std::ranges::sort(out, {}, &model::OrderData::orderid); // deterministic order
    return out;
}

auto ExecutionEngine::get_order(const std::string& orderid) -> model::OrderData* {
    auto it = orders_.find(orderid);
    return (it != orders_.end()) ? &it->second : nullptr;
}

auto ExecutionEngine::get_order(const std::string& orderid) const -> const model::OrderData* {
    auto it = orders_.find(orderid);
    return (it != orders_.end()) ? &it->second : nullptr;
}

auto ExecutionEngine::get_trade(const std::string& tradeid) -> model::TradeData* {
    auto it = trades_.find(tradeid);
    return (it != trades_.end()) ? &it->second : nullptr;
}

auto ExecutionEngine::get_strategy_name_for_order(const std::string& orderid) const -> std::string {
    auto it = orderid_strategy_name_.find(orderid);
    return (it != orderid_strategy_name_.end()) ? it->second : std::string{};
}

auto ExecutionEngine::get_all_orders() const -> std::vector<model::OrderData> {
    std::vector<model::OrderData> out;
    out.reserve(orders_.size());
    for (const auto& [_, order] : orders_) {
        out.push_back(order);
    }
    return out;
}

auto ExecutionEngine::get_all_trades() const -> std::vector<model::TradeData> {
    std::vector<model::TradeData> out;
    out.reserve(trades_.size());
    for (const auto& [_, trade] : trades_) {
        out.push_back(trade);
    }
    return out;
}

auto ExecutionEngine::get_all_active_orders() const -> std::vector<model::OrderData> {
    std::vector<model::OrderData> out;
    for (const auto& [_, orderids] : strategy_active_orders_) {
        for (const std::string& oid : orderids) {
            auto it = orders_.find(oid);
            if (it != orders_.end() && it->second.is_active()) {
                out.push_back(it->second);
            }
        }
    }
    return out;
}

auto ExecutionEngine::active_orders(const std::string& strategy_name) const
    -> const std::set<std::string>& {
    static const std::set<std::string> kNone;
    const auto it = strategy_active_orders_.find(strategy_name);
    return it != strategy_active_orders_.end() ? it->second : kNone;
}

auto ExecutionEngine::is_working(const std::string& orderid) const -> bool {
    const auto it = orderid_strategy_name_.find(orderid);
    return it != orderid_strategy_name_.end() && active_orders(it->second).contains(orderid);
}

void ExecutionEngine::remove_order_tracking(const std::string& orderid) {
    // No longer working; whose it is stays known, for its last status and a fill that comes late.
    auto it = orderid_strategy_name_.find(orderid);
    if (it != orderid_strategy_name_.end()) {
        if (const auto active = strategy_active_orders_.find(it->second);
            active != strategy_active_orders_.end()) {
            active->second.erase(orderid);
        }
    }
}

void ExecutionEngine::restore_order(const std::string& strategy_name,
                                    const model::OrderData& order) {
    add_order(order);
    orderid_strategy_name_[order.orderid] = strategy_name;
    if (order.is_active()) {
        strategy_active_orders_[strategy_name].insert(order.orderid);
    }
}

void ExecutionEngine::remove_strategy_tracking(const std::string& strategy_name) {
    std::set<std::string> orderids;
    for (auto it = orderid_strategy_name_.begin(); it != orderid_strategy_name_.end();) {
        if (it->second == strategy_name) {
            orderids.insert(it->first);
            it = orderid_strategy_name_.erase(it);
        } else {
            ++it;
        }
    }
    for (const std::string& oid : orderids) {
        orders_.erase(oid);
        filled_volume_.erase(oid);
        deferred_updates_.erase(oid);
    }
    std::erase_if(trades_, [&](const auto& kv) { return orderids.contains(kv.second.orderid); });
    strategy_active_orders_.erase(strategy_name);
}

void ExecutionEngine::clear() {
    orders_.clear();
    trades_.clear();
    filled_volume_.clear();
    deferred_updates_.clear();
    strategy_active_orders_.clear();
    orderid_strategy_name_.clear();
}

} // namespace core
