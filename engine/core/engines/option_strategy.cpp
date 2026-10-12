#include "core/engines/option_strategy.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"
#include "model/event.hpp"
#include "model/utility.hpp"
#include "model/portfolio_structure.hpp"
#include "core/engines/execution.hpp"
#include "core/engines/hedge.hpp"
#include "core/engines/position.hpp"
#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace core {

OptionStrategyEngine::OptionStrategyEngine(utilities::Logger* logger,
                                           model::PortfolioStructure& portfolios,
                                           ExecutionEngine& execution, PositionEngine& positions,
                                           HedgeEngine& hedge, RuntimeAPI& runtime)
    : BaseEngine(logger, "OptionStrategy"), portfolios_(portfolios), execution_(execution),
      positions_(positions), hedge_(hedge), runtime_(runtime) {}

OptionStrategyEngine::~OptionStrategyEngine() = default;

void OptionStrategyEngine::process_order_event(model::OrderData& order) {
    const std::string strategy_name = execution_.get_strategy_name_for_order(order.orderid);
    if (!strategy_name.empty()) {
        auto* s = get_strategy(strategy_name);
        if (s != nullptr) {
            s->on_order(order);
        }
    }
}

void OptionStrategyEngine::process_trade_event(const model::TradeData& trade) {
    const std::string strategy_name = execution_.get_strategy_name_for_order(trade.orderid);
    if (!strategy_name.empty()) {
        auto* s = get_strategy(strategy_name);
        if (s != nullptr) {
            s->on_trade(trade);
        }
    }
}

auto OptionStrategyEngine::get_strategy(const std::string& strategy_name)
    -> strategy::OptionStrategyTemplate* {
    auto it = strategies_.find(strategy_name);
    return (it != strategies_.end()) ? it->second.get() : nullptr;
}

auto OptionStrategyEngine::get_strategy() -> strategy::OptionStrategyTemplate* {
    if (strategies_.size() != 1) {
        return nullptr;
    }
    return strategies_.begin()->second.get();
}

auto OptionStrategyEngine::get_strategy_holding(const std::string& strategy_name) const
    -> model::StrategyHolding* {
    return &positions_.get_holding(strategy_name);
}

auto OptionStrategyEngine::get_strategy_holding() const -> model::StrategyHolding* {
    if (strategies_.size() != 1) {
        return nullptr;
    }
    return get_strategy_holding(strategies_.begin()->first);
}

auto OptionStrategyEngine::get_portfolio(const std::string& portfolio_name) const
    -> model::PortfolioData* {
    return portfolios_.get_portfolio(portfolio_name);
}

auto OptionStrategyEngine::get_contract(const std::string& symbol) const
    -> const model::ContractData* {
    return portfolios_.get_contract(symbol);
}

void OptionStrategyEngine::write_log(const std::string& strategy_name, const std::string& msg,
                                     utilities::LogLevel level) const {
    utilities::BaseEngine::write_log(msg, level, strategy_name);
}

auto OptionStrategyEngine::send_order(const std::string& strategy_name, const std::string& symbol,
                                      model::Direction direction, double price, double volume,
                                      model::OrderType order_type) -> std::vector<std::string> {
    model::OrderRequest req;
    if (!assemble_order_request(strategy_name, symbol, direction, price, volume, order_type,
                                std::span<const model::Leg>{}, std::nullopt, nullptr, req)) {
        write_log(strategy_name, std::format("No contract {}: order not sent", symbol),
                  utilities::WARNING);
        return {};
    }
    std::string orderid = runtime_.send_order(strategy_name, req);
    return orderid.empty() ? std::vector<std::string>{} : std::vector<std::string>{orderid};
}

auto OptionStrategyEngine::send_combo_order(const std::string& strategy_name,
                                            model::ComboType combo_type,
                                            const std::string& combo_sig,
                                            model::Direction direction, double price,
                                            double volume, std::span<const model::Leg> legs,
                                            model::OrderType order_type)
    -> std::vector<std::string> {
    if (legs.empty()) {
        return {};
    }
    model::OrderRequest req;
    if (!assemble_order_request(strategy_name, /*symbol unused*/ "", direction, price, volume,
                                order_type, legs, combo_type, &combo_sig, req)) {
        return {};
    }
    std::string orderid = runtime_.send_order(strategy_name, req);
    return orderid.empty() ? std::vector<std::string>{} : std::vector<std::string>{orderid};
}

auto OptionStrategyEngine::assemble_order_request(
    const std::string& strategy_name, const std::string& symbol, model::Direction direction,
    double price, double volume, model::OrderType order_type,
    std::span<const model::Leg> legs, std::optional<model::ComboType> combo_type,
    const std::string* combo_sig, model::OrderRequest& out_req) const -> bool {
    // Combo order
    if (!legs.empty() && combo_type.has_value() && combo_sig != nullptr) {
        // ComboType prefix
        std::string prefix = model::to_string(*combo_type); // e.g. "straddle", "iron_condor"
        out_req.symbol = prefix + "_" + *combo_sig;
        out_req.direction = direction;
        out_req.type = order_type;
        out_req.volume = volume;
        out_req.price =
            (order_type == model::OrderType::MARKET) ? 0.0 : model::round_to(price, 0.01);
        out_req.is_combo = true;
        out_req.combo_type = *combo_type;
        out_req.legs.assign(legs.begin(), legs.end());
        out_req.trading_class = legs.front().trading_class;
        out_req.reference = "Strategy_" + strategy_name;
        return true;
    }

    // Single leg
    const model::ContractData* contract = get_contract(symbol);
    if (contract == nullptr) {
        return false;
    }
    out_req.symbol = contract->symbol;
    out_req.direction = direction;
    out_req.type = order_type;
    out_req.volume = model::round_to(volume, contract->min_volume);
    out_req.price =
        (order_type == model::OrderType::MARKET) ? 0.0 : model::round_to(price, 0.01);
    out_req.reference = "Strategy_" + strategy_name;
    out_req.trading_class = contract->trading_class;
    out_req.is_combo = false;
    out_req.legs.clear();
    return true;
}

void OptionStrategyEngine::init_strategy(const std::string& strategy_name) {
    auto* s = get_strategy(strategy_name);
    if (s == nullptr) {
        throw std::runtime_error("Strategy not found: " + strategy_name);
    }
    s->on_init();
}

void OptionStrategyEngine::start_strategy(const std::string& strategy_name) {
    auto* s = get_strategy(strategy_name);
    if (s == nullptr) {
        throw std::runtime_error("Strategy not found: " + strategy_name);
    }
    s->on_start();
}

void OptionStrategyEngine::stop_strategy(const std::string& strategy_name) {
    auto* s = get_strategy(strategy_name);
    if (s == nullptr) {
        throw std::runtime_error("Strategy not found: " + strategy_name);
    }
    s->on_stop();
}

auto OptionStrategyEngine::remove_strategy(const std::string& strategy_name) -> bool {
    auto it = strategies_.find(strategy_name);
    if (it == strategies_.end()) {
        return false;
    }
    if (const model::StrategyHolding* holding = positions_.find_holding(strategy_name);
        holding != nullptr && !holding->is_flat()) {
        throw std::runtime_error(strategy_name + " holds positions: close them first");
    }
    if (has_active_order(strategy_name)) {
        throw std::runtime_error(strategy_name + " has orders working: cancel them first");
    }
    if (it->second) {
        it->second->on_stop();
    }
    execution_.remove_strategy_tracking(strategy_name);
    strategies_.erase(it);
    std::erase(strategy_names_, strategy_name);
    positions_.remove_strategy_holding(strategy_name);
    runtime_.unsubscribe_chains(strategy_name);
    return true;
}

void OptionStrategyEngine::subscribe_chains(const std::string& strategy_name,
                                            std::span<const std::string> chains) {
    runtime_.subscribe_chains(strategy_name, chains);
}

auto OptionStrategyEngine::has_active_order(const std::string& strategy_name) const -> bool {
    return std::ranges::any_of(execution_.active_orders(strategy_name),
                               [this](const std::string& orderid) -> bool {
        const model::OrderData* order = execution_.get_order(orderid);
        return order != nullptr && order->is_active();
    });
}

void OptionStrategyEngine::register_hedging(const std::string& strategy_name,
                                            const std::string& portfolio_name, int timer_trigger,
                                            int delta_target, int delta_range) {
    hedge_.register_strategy(strategy_name, portfolio_name, timer_trigger, delta_target,
                             delta_range);
    const model::PortfolioData* portfolio = get_portfolio(portfolio_name);
    if (portfolio != nullptr && portfolio->hedge() == nullptr) {
        write_log(
            std::format("{} hedges {}, which has no {} contract to hedge with: sync the "
                        "contracts",
                        strategy_name, portfolio_name,
                        portfolio->hedge_symbol.empty() ? "underlying" : portfolio->hedge_symbol),
            utilities::WARNING);
    }
}

void OptionStrategyEngine::unregister_hedging(const std::string& strategy_name) {
    hedge_.unregister_strategy(strategy_name);
}

std::unordered_map<std::string, double>
OptionStrategyEngine::get_default_setting(const std::string& class_name) const {
    return strategy::StrategyRegistry::resolve_setting(class_name, {});
}

auto OptionStrategyEngine::get_strategy_class_name(const std::string& strategy_name) const
    -> std::string {
    auto it = strategies_.find(strategy_name);
    return it != strategies_.end() && it->second ? it->second->class_name() : std::string{};
}

auto OptionStrategyEngine::add_strategy(const std::string& class_name,
                                        const std::string& portfolio_name,
                                        const std::unordered_map<std::string, double>& setting,
                                        const std::string& strategy_name) -> std::string {
    // A strategy's orders and trades are kept under its name, and a restart books them again by
    // it: a name is never given to another strategy.
    const auto used_before = [this](const std::string& name) {
        return runtime_.is_strategy_name_used(name);
    };
    std::string name = strategy_name;
    if (name.empty()) {
        const std::string base = class_name + "_" + portfolio_name;
        name = base;
        for (int n = 2; strategies_.contains(name) || used_before(name); ++n) {
            name = std::format("{}_{}", base, n);
        }
    } else if (strategies_.contains(name)) {
        throw std::runtime_error(
            std::format("Strategy '{}' already exists; remove it or add under another name", name));
    } else if (used_before(name)) {
        throw std::invalid_argument(
            std::format("{} was used before, and its orders and trades are kept under that name:"
                        " restore it if it was removed, or choose another name",
                        name));
    }
    create_strategy(class_name, portfolio_name, setting, name);
    return name;
}

void OptionStrategyEngine::restore_strategy(const std::string& class_name,
                                            const std::string& portfolio_name,
                                            const std::unordered_map<std::string, double>& setting,
                                            const std::string& strategy_name) {
    if (strategies_.contains(strategy_name)) {
        throw std::runtime_error(std::format("Strategy '{}' already exists", strategy_name));
    }
    create_strategy(class_name, portfolio_name, setting, strategy_name);
}

void OptionStrategyEngine::create_strategy(const std::string& class_name,
                                           const std::string& portfolio_name,
                                           const std::unordered_map<std::string, double>& setting,
                                           const std::string& name) {
    // Throws on an unknown class or a setting the class does not declare or allow.
    const std::unordered_map<std::string, double> resolved =
        strategy::StrategyRegistry::resolve_setting(class_name, setting);
    std::unique_ptr<strategy::OptionStrategyTemplate> ptr =
        strategy::StrategyRegistry::create(class_name, this, name, portfolio_name, resolved);
    positions_.get_create_strategy_holding(name);
    ptr->set_holding(&positions_.get_holding(name));
    strategies_[name] = std::move(ptr);
    strategy_names_.push_back(name);
}

void OptionStrategyEngine::process_timer_event() {
    for (auto& [_, s] : strategies_) {
        if (s) {
            s->on_timer();
        }
    }
}

void OptionStrategyEngine::process_snapshot_event(const std::string& portfolio_name) {
    for (auto& [_, s] : strategies_) {
        if (s && s->started() && !s->error() && s->portfolio_name() == portfolio_name) {
            s->on_snapshot();
        }
    }
}

auto OptionStrategyEngine::get_strategy_errors() const
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [name, ptr] : strategies_) {
        if (ptr && ptr->error()) {
            out.emplace_back(name, ptr->error_msg());
        }
    }
    return out;
}

void OptionStrategyEngine::close() {
    // No on_stop: closing the engine is not stopping its strategies. A strategy's on_stop may close
    // its positions; on a restart the strategy resumes with them as saved.
    strategies_.clear();
    strategy_names_.clear();
}

} // namespace core
