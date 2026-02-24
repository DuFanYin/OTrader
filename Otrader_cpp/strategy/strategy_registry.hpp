#pragma once

/**
 * 策略注册表：统一管理「策略名 → 工厂」。
 * 新策略只需在 strategy_registry.cpp 顶部加 include、末尾加一行 REGISTER_STRATEGY(YourClassName);
 * 使用宏的 TU 需能见到 OptionStrategyEngine 完整类型（本库 strategy_registry 已配 core 头路径）。
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
    void* engine,
    const std::string& strategy_name,
    const std::string& portfolio_name,
    const std::unordered_map<std::string, double>& setting)>;

class StrategyRegistry {
public:
    static void add(const std::string& class_name);
    static void add_factory(const std::string& class_name, StrategyFactoryFunc factory);
    static bool has(const std::string& class_name);
    static std::vector<std::string> get_all_strategy_class_names();
    static void* create(const std::string& class_name,
                        void* engine,
                        const std::string& strategy_name,
                        const std::string& portfolio_name,
                        const std::unordered_map<std::string, double>& setting);
};

/** 在 strategy_registry.cpp 中集中使用，例如：REGISTER_STRATEGY(HighFrequencyMomentumStrategy); */
#define REGISTER_STRATEGY(ClassName) \
    static bool _reg_##ClassName = (strategy_cpp::StrategyRegistry::add_factory(#ClassName, \
        [](void* e, const std::string& sn, const std::string& pn, const std::unordered_map<std::string, double>& s) -> void* { \
            return new ClassName(static_cast<core::OptionStrategyEngine*>(e), sn, pn, s); \
        }), true)

}  // namespace strategy_cpp
