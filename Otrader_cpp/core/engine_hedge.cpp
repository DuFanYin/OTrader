/**
 * Shared HedgeEngine (from engines/engine_hedge.py).
 * Same logic: timer, run_strategy_hedging, compute_hedge_plan, execute_hedge_orders, check/cancel
 * orders.
 */

#include "engine_hedge.hpp"
#include <cmath>

namespace engines {

static constexpr const char* APP_NAME = "Hedge";

void HedgeEngine::register_strategy(const std::string& strategy_name, int timer_trigger,
                                    int delta_target, int delta_range) {
    HedgeConfig config;
    config.strategy_name = strategy_name;
    config.timer_trigger = timer_trigger;
    config.delta_target = delta_target;
    config.delta_range = delta_range;
    registered_strategies_[strategy_name] = config;
}

void HedgeEngine::unregister_strategy(const std::string& strategy_name) {
    registered_strategies_.erase(strategy_name);
}

void HedgeEngine::process_hedging(const std::string& strategy_name, const HedgeParams& params,
                                  std::vector<utilities::OrderRequest>* out_orders,
                                  std::vector<utilities::CancelRequest>* out_cancels,
                                  std::vector<utilities::LogData>* out_logs) {
    if ((out_orders == nullptr) && (out_cancels == nullptr) && (out_logs == nullptr)) {
        return;
    }
    auto it = registered_strategies_.find(strategy_name);
    if (it == registered_strategies_.end()) {
        return;
    }
    run_strategy_hedging_with_params(strategy_name, it->second, params, out_orders, out_cancels,
                                     out_logs);
}

void HedgeEngine::run_strategy_hedging_with_params(
    const std::string& strategy_name, HedgeConfig& config, const HedgeParams& params,
    std::vector<utilities::OrderRequest>* out_orders,
    std::vector<utilities::CancelRequest>* out_cancels, std::vector<utilities::LogData>* out_logs) {
    if (!check_strategy_orders_finished(strategy_name, params)) {
        cancel_strategy_orders(strategy_name, params, out_cancels);
        return;
    }
    auto plan = compute_hedge_plan(strategy_name, config, params);
    if (!plan.has_value()) {
        return;
    }
    auto [symbol, direction, available, order_volume] = plan.value();
    execute_hedge_orders(strategy_name, symbol, direction, available, order_volume, params,
                         out_orders, out_logs);
}

auto HedgeEngine::compute_hedge_plan(const std::string& strategy_name, HedgeConfig& config,
                                     const HedgeParams& params)
    -> std::optional<std::tuple<std::string, utilities::Direction, double, double>> {
    (void)config;
    if ((params.holding == nullptr) || (params.portfolio == nullptr) ||
        !params.portfolio->underlying) {
        return std::nullopt;
    }

    double total_delta = params.holding->summary.delta;
    double delta_max = config.delta_target + config.delta_range;
    double delta_min = config.delta_target - config.delta_range;
    if (total_delta >= delta_min && total_delta <= delta_max) {
        return std::nullopt;
    }

    double delta_to_hedge = config.delta_target - total_delta;
    utilities::UnderlyingData& underlying = *params.portfolio->underlying;
    double hedge_volume =
        delta_to_hedge / (underlying.theo_delta != 0 ? underlying.theo_delta : 1.0);
    std::string symbol = underlying.symbol;

    if (!params.get_contract) {
        return std::nullopt;
    }
    const utilities::ContractData* contract = params.get_contract(symbol);
    if ((contract == nullptr) || std::abs(hedge_volume) < 1) {
        return std::nullopt;
    }

    int qty = params.holding->underlyingPosition.quantity;
    utilities::Direction direction;
    double available;
    if (hedge_volume > 0) {
        direction = utilities::Direction::LONG;
        available = qty < 0 ? std::abs(qty) : 0;
    } else {
        direction = utilities::Direction::SHORT;
        available = qty > 0 ? static_cast<double>(qty) : 0;
    }
    return std::make_tuple(symbol, direction, available, std::abs(hedge_volume));
}

void HedgeEngine::execute_hedge_orders(const std::string& strategy_name, const std::string& symbol,
                                       utilities::Direction direction, double available,
                                       double order_volume, const HedgeParams& params,
                                       std::vector<utilities::OrderRequest>* out_orders,
                                       std::vector<utilities::LogData>* out_logs) {
    if ((out_orders == nullptr) && (out_logs == nullptr)) {
        return;
    }
    double remaining = order_volume;
    if (available > 0) {
        double close_vol = std::min(available, order_volume);
        submit_hedge_order(strategy_name, symbol, direction, close_vol, params, out_orders,
                           out_logs);
        remaining -= close_vol;
    }
    if (remaining > 0) {
        submit_hedge_order(strategy_name, symbol, direction, remaining, params, out_orders,
                           out_logs);
    }
}

void HedgeEngine::submit_hedge_order(const std::string& strategy_name, const std::string& symbol,
                                     utilities::Direction direction, double volume,
                                     const HedgeParams& params,
                                     std::vector<utilities::OrderRequest>* out_orders,
                                     std::vector<utilities::LogData>* out_logs) {
    if (!params.get_contract) {
        return;
    }
    const utilities::ContractData* contract = params.get_contract(symbol);
    if (contract == nullptr) {
        return;
    }
    utilities::OrderRequest req;
    req.symbol = contract->symbol;
    req.exchange = contract->exchange;
    req.direction = direction;
    req.type = utilities::OrderType::MARKET;
    req.volume = volume;
    req.price = 0.0;
    req.reference = std::string("Hedge_") + strategy_name;
    req.trading_class = contract->trading_class;
    if (out_orders != nullptr) {
        out_orders->push_back(req);
    }

    if (out_logs != nullptr) {
        utilities::LogData log;
        log.msg = std::string("Hedge sending order: dir=") +
                  (direction == utilities::Direction::LONG ? "LONG" : "SHORT") +
                  ", vol=" + std::to_string(volume) + ", symbol=" + symbol;
        log.level = 0;
        log.gateway_name = APP_NAME;
        out_logs->push_back(log);
    }
}

auto HedgeEngine::check_strategy_orders_finished(const std::string& strategy_name,
                                                 const HedgeParams& params) -> bool {
    if (!params.get_strategy_active_orders || !params.get_order) {
        return true;
    }
    const auto& active_map = params.get_strategy_active_orders();
    auto it = active_map.find(strategy_name);
    if (it == active_map.end()) {
        return true;
    }
    for (const auto& orderid : it->second) {
        utilities::OrderData* order = params.get_order(orderid);
        if ((order != nullptr) && order->reference.find(APP_NAME) != std::string::npos) {
            return false;
        }
    }
    return true;
}

void HedgeEngine::cancel_strategy_orders(const std::string& strategy_name,
                                         const HedgeParams& params,
                                         std::vector<utilities::CancelRequest>* out_cancels) {
    if ((out_cancels == nullptr) || !params.get_strategy_active_orders || !params.get_order) {
        return;
    }
    const auto& active_map = params.get_strategy_active_orders();
    auto it = active_map.find(strategy_name);
    if (it == active_map.end()) {
        return;
    }
    for (const auto& orderid : it->second) {
        utilities::OrderData* order = params.get_order(orderid);
        if ((order != nullptr) && order->reference.find(APP_NAME) != std::string::npos) {
            out_cancels->push_back(order->create_cancel_request());
        }
    }
}

} // namespace engines
