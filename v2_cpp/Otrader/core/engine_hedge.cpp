/**
 * Shared HedgeEngine (from engines/engine_hedge.py).
 * Same logic: timer, run_strategy_hedging, compute_hedge_plan, execute_hedge_orders, check/cancel
 * orders.
 */

#include "engine_hedge.hpp"

#include <cmath>
#include <math.h>
#include <utility>

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
                                  std::vector<utilities::CancelRequest>* out_cancels) {
    if ((out_orders == nullptr) && (out_cancels == nullptr)) {
        return;
    }
    auto it = registered_strategies_.find(strategy_name);
    if (it == registered_strategies_.end()) {
        return;
    }
    run_strategy_hedging_with_params(strategy_name, it->second, params, out_orders, out_cancels);
}

namespace {

inline std::string default_strategy_to_portfolio_name(const std::string& strategy_name) {
    std::string portfolio_name = strategy_name;
    const size_t p = strategy_name.find('_');
    if (p != std::string::npos && p + 1 < strategy_name.size()) {
        portfolio_name = strategy_name.substr(p + 1);
    }
    return portfolio_name;
}

} // namespace

void HedgeEngine::process_timer_event() {
    if (!api_.system.get_strategy_names_ref || !api_.portfolio.get_portfolio ||
        !api_.portfolio.get_holding || !api_.portfolio.get_contract ||
        !api_.execution.get_strategy_active_orders || !api_.execution.get_order ||
        !api_.execution.send_order || !api_.execution.cancel_order) {
        return;
    }

    const auto& strategy_names = api_.system.get_strategy_names_ref();
    if (strategy_names.empty()) {
        return;
    }

    std::unordered_map<std::string, utilities::PortfolioData*> portfolio_cache;
    portfolio_cache.reserve(strategy_names.size());

    std::vector<utilities::OrderRequest> orders;
    std::vector<utilities::CancelRequest> cancels;

    for (const std::string& strategy_name : strategy_names) {
        const std::string portfolio_name = default_strategy_to_portfolio_name(strategy_name);
        utilities::PortfolioData* portfolio_ptr = nullptr;
        if (auto it = portfolio_cache.find(portfolio_name); it != portfolio_cache.end()) {
            portfolio_ptr = it->second;
        } else {
            portfolio_ptr = api_.portfolio.get_portfolio(portfolio_name);
            portfolio_cache.emplace(portfolio_name, portfolio_ptr);
        }

        engines::HedgeParams params;
        params.portfolio = portfolio_ptr;
        params.holding = api_.portfolio.get_holding(strategy_name);
        params.get_contract = api_.portfolio.get_contract;
        params.get_strategy_active_orders = api_.execution.get_strategy_active_orders;
        params.get_order = api_.execution.get_order;

        orders.clear();
        cancels.clear();
        process_hedging(strategy_name, params, &orders, &cancels);

        for (const auto& o : orders) {
            (void)api_.execution.send_order(strategy_name, o);
        }
        for (const auto& c : cancels) {
            api_.execution.cancel_order(c);
        }
        // Per-order string logs are emitted inside submit_hedge_order via BaseEngine::write_log.
    }
}

void HedgeEngine::run_strategy_hedging_with_params(
    const std::string& strategy_name, HedgeConfig& config, const HedgeParams& params,
    std::vector<utilities::OrderRequest>* out_orders,
    std::vector<utilities::CancelRequest>* out_cancels) {
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
                         out_orders);
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
    utilities::Direction direction =
        (hedge_volume > 0) ? utilities::Direction::LONG : utilities::Direction::SHORT;
    double available = (hedge_volume > 0) ? (qty < 0 ? std::abs(qty) : 0.0)
                                          : (qty > 0 ? static_cast<double>(qty) : 0.0);
    return std::make_tuple(symbol, direction, available, std::abs(hedge_volume));
}

void HedgeEngine::execute_hedge_orders(const std::string& strategy_name, const std::string& symbol,
                                       utilities::Direction direction, double available,
                                       double order_volume, const HedgeParams& params,
                                       std::vector<utilities::OrderRequest>* out_orders) {
    if (out_orders == nullptr) {
        return;
    }
    double remaining = order_volume;
    if (available > 0) {
        double close_vol = std::min(available, order_volume);
        submit_hedge_order(strategy_name, symbol, direction, close_vol, params, out_orders);
        remaining -= close_vol;
    }
    if (remaining > 0) {
        submit_hedge_order(strategy_name, symbol, direction, remaining, params, out_orders);
    }
}

void HedgeEngine::submit_hedge_order(const std::string& strategy_name, const std::string& symbol,
                                     utilities::Direction direction, double volume,
                                     const HedgeParams& params,
                                     std::vector<utilities::OrderRequest>* out_orders) {
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

    // Unified core logging path: string-based, via BaseEngine.
    write_log(std::string("Hedge sending order: dir=") +
                  (direction == utilities::Direction::LONG ? "LONG" : "SHORT") +
                  ", vol=" + std::to_string(volume) + ", symbol=" + symbol,
              0, APP_NAME);
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
