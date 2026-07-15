/**
 * Strategy registry. Strategies registered here (REGISTER_STRATEGY); registration runs when TU
 * linked. add_strategy errors on unknown strategy name (OptionStrategyEngine::add_strategy).
 */

#include "strategy_registry.hpp"
#include "engine_option_strategy.hpp"
#include "ironcondortest.hpp"
#include "iv_mean_revert.hpp"
#include "straddle_inventory_scalper.hpp"
#include "straddletest.hpp"
#include <mutex>
#include <ranges>

namespace strategy_cpp {

namespace {

struct Registry {
    std::vector<std::string> names; // Stable order for get_all
    std::unordered_map<std::string, StrategyFactoryFunc> factories;
    std::mutex mtx;
};
auto registry() -> Registry& {
    static Registry r;
    return r;
}

} // namespace

void StrategyRegistry::add(const std::string& class_name) {
    std::scoped_lock lock(registry().mtx);
    if (std::ranges::find(registry().names, class_name) == registry().names.end()) {
        registry().names.push_back(class_name);
    }
}

void StrategyRegistry::add_factory(const std::string& class_name, StrategyFactoryFunc factory) {
    std::scoped_lock lock(registry().mtx);
    registry().factories[class_name] = std::move(factory);
    if (std::ranges::find(registry().names, class_name) == registry().names.end()) {
        registry().names.push_back(class_name);
    }
}

auto StrategyRegistry::has(const std::string& class_name) -> bool {
    std::scoped_lock lock(registry().mtx);
    return std::ranges::find(registry().names, class_name) != registry().names.end();
}

auto StrategyRegistry::get_all_strategy_class_names() -> std::vector<std::string> {
    std::scoped_lock lock(registry().mtx);
    return registry().names;
}

auto StrategyRegistry::create(const std::string& class_name, void* engine,
                              const std::string& strategy_name, const std::string& portfolio_name,
                              const std::unordered_map<std::string, double>& setting) -> void* {
    std::scoped_lock lock(registry().mtx);
    auto it = registry().factories.find(class_name);
    if (it == registry().factories.end() || !it->second) {
        return nullptr;
    }
    return it->second(engine, strategy_name, portfolio_name, setting);
}

// Strategy registry
REGISTER_STRATEGY(StraddleTestStrategy);
REGISTER_STRATEGY(IvMeanRevertStrategy);
REGISTER_STRATEGY(IronCondorTestStrategy);
REGISTER_STRATEGY(StraddleInventoryScalperStrategy);

} // namespace strategy_cpp
