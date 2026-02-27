/**
 * 策略注册表。可用策略在此集中登记（一行 REGISTER_STRATEGY），保证本 TU 被链接时注册一定执行；
 * add_strategy 若传入不存在的策略名会直接报错（见 core OptionStrategyEngine::add_strategy）。
 */

#include "strategy_registry.hpp"
#include "engine_option_strategy.hpp"
#include "high_frequency_momentum.hpp"
#include "ironcondor.hpp"
#include "iv_mean_revert.hpp"
#include <algorithm>
#include <mutex>

namespace strategy_cpp {

namespace {

struct Registry {
    std::vector<std::string> names; // 顺序稳定，用于 get_all
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
    auto it = std::find(registry().names.begin(), registry().names.end(), class_name);
    if (it == registry().names.end()) {
        registry().names.push_back(class_name);
}
}

void StrategyRegistry::add_factory(const std::string& class_name, StrategyFactoryFunc factory) {
    std::scoped_lock lock(registry().mtx);
    registry().factories[class_name] = std::move(factory);
    auto it = std::find(registry().names.begin(), registry().names.end(), class_name);
    if (it == registry().names.end()) {
        registry().names.push_back(class_name);
}
}

auto StrategyRegistry::has(const std::string& class_name) -> bool {
    std::scoped_lock lock(registry().mtx);
    return std::find(registry().names.begin(), registry().names.end(), class_name) !=
           registry().names.end();
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

// 集中登记：新策略在此加一行 REGISTER_STRATEGY(ClassName); 并在本文件顶部 include 对应头文件
REGISTER_STRATEGY(HighFrequencyMomentumStrategy);
REGISTER_STRATEGY(IvMeanRevertStrategy);
REGISTER_STRATEGY(IronCondorStrategy);

} // namespace strategy_cpp
