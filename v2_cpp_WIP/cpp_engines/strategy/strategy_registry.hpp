#pragma once

/**
 * Strategy registry: strategy name → factory.
 * Add include + REGISTER_STRATEGY(YourClassName) in strategy_registry.cpp.
 * TU using macro must see full OptionStrategyEngine (strategy_registry has core include).
 */

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {
class OptionStrategyEngine;
}

namespace strategy_cpp {

using StrategyFactoryFunc = std::function<void*(
    void* engine, const std::string& strategy_name, const std::string& portfolio_name,
    const std::unordered_map<std::string, double>& setting)>;

class StrategyRegistry {
  public:
    static void add(const std::string& class_name);
    static void add_factory(const std::string& class_name, StrategyFactoryFunc factory);
    static bool has(const std::string& class_name);
    static std::vector<std::string> get_all_strategy_class_names();
    static void* create(const std::string& class_name, void* engine,
                        const std::string& strategy_name, const std::string& portfolio_name,
                        const std::unordered_map<std::string, double>& setting);
};

/** Use in strategy_registry.cpp, e.g. REGISTER_STRATEGY(StraddleTestStrategy). */
#define REGISTER_STRATEGY(ClassName)                                                               \
    static bool _reg_##ClassName =                                                                 \
        (strategy_cpp::StrategyRegistry::add_factory(                                              \
             #ClassName,                                                                           \
             [](void* e, const std::string& sn, const std::string& pn,                             \
                const std::unordered_map<std::string, double>& s) -> void* {                       \
                 return new ClassName(static_cast<core::OptionStrategyEngine*>(e), sn, pn, s);     \
             }),                                                                                   \
         true)

} // namespace strategy_cpp
