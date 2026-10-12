#pragma once

/**
 * Strategy registry: strategy class name → factory and declared parameters.
 * A strategy registers itself: REGISTER_STRATEGY(YourClassName) in its own .cpp under factory/
 * (CMake picks the file up and links every strategy in; see strategy/CMakeLists.txt).
 */

#include "strategy_param.hpp"
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace strategy {

class OptionStrategyTemplate;
class StrategyContext;

using StrategyFactory = std::function<std::unique_ptr<OptionStrategyTemplate>(
    StrategyContext* context, const std::string& strategy_name, const std::string& portfolio_name,
    const std::unordered_map<std::string, double>& setting)>;

class StrategyRegistry {
  public:
    /** Register a class (REGISTER_STRATEGY does); returns true. */
    static bool add(const std::string& class_name, StrategyFactory factory,
                    std::span<const StrategyParam> parameters);
    static bool has(const std::string& class_name);
    /** Registered class names, sorted. */
    static std::vector<std::string> get_all_strategy_class_names();
    /** The class's parameters: the template's, then its own. Throws on an unknown class. */
    static std::vector<StrategyParam> parameters(const std::string& class_name);
    /** The declared defaults, overridden by `setting`. Throws std::invalid_argument on an unknown
     * class, an undeclared name, a value out of range or a fraction for an integer. */
    static std::unordered_map<std::string, double>
    resolve_setting(const std::string& class_name,
                    const std::unordered_map<std::string, double>& setting);
    /** A new instance; `setting` comes from resolve_setting. Throws on an unknown class. */
    static std::unique_ptr<OptionStrategyTemplate>
    create(const std::string& class_name, StrategyContext* context,
           const std::string& strategy_name, const std::string& portfolio_name,
           const std::unordered_map<std::string, double>& setting);
};

/** In a strategy's .cpp, inside namespace strategy: REGISTER_STRATEGY(IvMeanRevertStrategy).
 * The class declares a static parameters() (the template's empty one if it has none). */
#define REGISTER_STRATEGY(ClassName)                                                               \
    [[maybe_unused]] static const bool registered_##ClassName =                                    \
        strategy::StrategyRegistry::add(                                                       \
            #ClassName,                                                                            \
            [](strategy::StrategyContext* context, const std::string& strategy_name,           \
               const std::string& portfolio_name,                                                  \
               const std::unordered_map<std::string, double>& setting)                             \
                -> std::unique_ptr<strategy::OptionStrategyTemplate> {                         \
                return std::make_unique<ClassName>(context, strategy_name, portfolio_name,         \
                                                   setting);                                       \
            },                                                                                     \
            ClassName::parameters())

} // namespace strategy
