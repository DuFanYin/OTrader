/**
 * Shared HedgeEngine (from engines/engine_hedge.py).
 * Same logic: timer, run_strategy_hedging, compute_hedge_plan, execute_hedge_orders, check/cancel
 * orders.
 */

#include "core/engines/hedge.hpp"
#include "model/portfolio_structure.hpp"
#include "core/engines/execution.hpp"
#include "core/engines/position.hpp"

#include <cmath>
#include <math.h>
#include <utility>

namespace core {

static constexpr const char* APP_NAME = "Hedge";

HedgeEngine::HedgeEngine(utilities::Logger* logger, model::PortfolioStructure& portfolios,
                         PositionEngine& positions, ExecutionEngine& execution,
                         RuntimeAPI& runtime)
    : BaseEngine(logger, "Hedge"), portfolios_(portfolios), positions_(positions),
      execution_(execution), runtime_(runtime) {}

void HedgeEngine::register_strategy(const std::string& strategy_name,
                                    const std::string& portfolio_name, int timer_trigger,
                                    int delta_target, int delta_range) {
    HedgeConfig config;
    config.strategy_name = strategy_name;
    config.portfolio_name = portfolio_name;
    config.timer_trigger = timer_trigger;
    config.delta_target = delta_target;
    config.delta_range = delta_range;
    registered_strategies_[strategy_name] = config;
}

void HedgeEngine::unregister_strategy(const std::string& strategy_name) {
    registered_strategies_.erase(strategy_name);
}

void HedgeEngine::process_hedging(const std::string& strategy_name,
                                  const model::PortfolioData& portfolio,
                                  const model::StrategyHolding& holding,
                                  std::vector<model::OrderRequest>* out_orders,
                                  std::vector<model::CancelRequest>* out_cancels) {
    if ((out_orders == nullptr) && (out_cancels == nullptr)) {
        return;
    }
    auto it = registered_strategies_.find(strategy_name);
    if (it == registered_strategies_.end()) {
        return;
    }
    run_strategy_hedging(strategy_name, it->second, portfolio, holding, out_orders, out_cancels);
}

void HedgeEngine::process_timer_event(const std::vector<std::string>& strategy_names) {
    std::vector<model::OrderRequest> orders;
    std::vector<model::CancelRequest> cancels;

    for (const std::string& strategy_name : strategy_names) {
        auto it = registered_strategies_.find(strategy_name);
        if (it == registered_strategies_.end()) {
            continue;
        }
        HedgeConfig& config = it->second;
        model::PortfolioData* portfolio = portfolios_.get_portfolio(config.portfolio_name);
        const model::StrategyHolding* holding = positions_.find_holding(strategy_name);
        if (portfolio == nullptr || holding == nullptr ||
            !config.bar_gate.next(portfolio->datetime()) ||
            ++config.timer_cnt < config.timer_trigger) {
            continue;
        }
        config.timer_cnt = 0;

        orders.clear();
        cancels.clear();
        run_strategy_hedging(strategy_name, config, *portfolio, *holding, &orders, &cancels);

        for (const auto& o : orders) {
            (void)runtime_.send_order(strategy_name, o);
        }
        for (const auto& c : cancels) {
            runtime_.cancel_order(c);
        }
    }
}

void HedgeEngine::run_strategy_hedging(const std::string& strategy_name,
                                       const HedgeConfig& config,
                                       const model::PortfolioData& portfolio,
                                       const model::StrategyHolding& holding,
                                       std::vector<model::OrderRequest>* out_orders,
                                       std::vector<model::CancelRequest>* out_cancels) {
    if (const std::vector<const model::OrderData*> working = working_hedges(strategy_name);
        !working.empty()) {
        if (out_cancels != nullptr) {
            for (const model::OrderData* order : working) {
                out_cancels->push_back(order->create_cancel_request());
            }
        }
        return;
    }
    auto plan = compute_hedge_plan(config, portfolio, holding);
    if (!plan.has_value()) {
        return;
    }
    auto [symbol, direction, available, order_volume] = plan.value();
    execute_hedge_orders(strategy_name, symbol, direction, available, order_volume, out_orders);
}

auto HedgeEngine::compute_hedge_plan(const HedgeConfig& config,
                                     const model::PortfolioData& portfolio,
                                     const model::StrategyHolding& holding) const
    -> std::optional<std::tuple<std::string, model::Direction, double, double>> {
    const model::UnderlyingData* instrument = portfolio.hedge();
    // theo_delta: the underlying's units per unit of the instrument; 0 until it has a price.
    if (instrument == nullptr || instrument->theo_delta <= 0.0) {
        return std::nullopt;
    }

    double total_delta = holding.summary.delta;
    double delta_max = config.delta_target + config.delta_range;
    double delta_min = config.delta_target - config.delta_range;
    if (total_delta >= delta_min && total_delta <= delta_max) {
        return std::nullopt;
    }

    double delta_to_hedge = config.delta_target - total_delta;
    double hedge_volume = delta_to_hedge / instrument->theo_delta;
    std::string symbol = instrument->symbol;

    // Whole units, the nearest: positions and the broker take no fraction of a share.
    const double units = std::round(std::abs(hedge_volume));
    if (portfolios_.get_contract(symbol) == nullptr || units < 1) {
        return std::nullopt;
    }

    int qty = holding.underlyingPosition.quantity;
    model::Direction direction =
        (hedge_volume > 0) ? model::Direction::LONG : model::Direction::SHORT;
    double available = (hedge_volume > 0) ? (qty < 0 ? std::abs(qty) : 0.0)
                                          : (qty > 0 ? static_cast<double>(qty) : 0.0);
    return std::make_tuple(symbol, direction, available, units);
}

void HedgeEngine::execute_hedge_orders(const std::string& strategy_name, const std::string& symbol,
                                       model::Direction direction, double available,
                                       double order_volume,
                                       std::vector<model::OrderRequest>* out_orders) {
    if (out_orders == nullptr) {
        return;
    }
    double remaining = order_volume;
    if (available > 0) {
        double close_vol = std::min(available, order_volume);
        submit_hedge_order(strategy_name, symbol, direction, close_vol, out_orders);
        remaining -= close_vol;
    }
    if (remaining > 0) {
        submit_hedge_order(strategy_name, symbol, direction, remaining, out_orders);
    }
}

void HedgeEngine::submit_hedge_order(const std::string& strategy_name, const std::string& symbol,
                                     model::Direction direction, double volume,
                                     std::vector<model::OrderRequest>* out_orders) {
    const model::ContractData* contract = portfolios_.get_contract(symbol);
    if (contract == nullptr) {
        return;
    }
    model::OrderRequest req;
    req.symbol = contract->symbol;
    req.direction = direction;
    req.type = model::OrderType::MARKET;
    req.volume = volume;
    req.price = 0.0;
    req.reference = std::string("Hedge_") + strategy_name;
    req.trading_class = contract->trading_class;
    if (out_orders != nullptr) {
        out_orders->push_back(req);
    }

    write_log(std::string("Hedge sending order: dir=") +
                  (direction == model::Direction::LONG ? "LONG" : "SHORT") +
                  ", vol=" + std::to_string(volume) + ", symbol=" + symbol,
              utilities::INFO);
}

auto HedgeEngine::working_hedges(const std::string& strategy_name) const
    -> std::vector<const model::OrderData*> {
    std::vector<const model::OrderData*> out;
    for (const std::string& orderid : execution_.active_orders(strategy_name)) {
        const model::OrderData* order = execution_.get_order(orderid);
        if (order != nullptr && order->reference.find(APP_NAME) != std::string::npos) {
            out.push_back(order);
        }
    }
    return out;
}

} // namespace core
