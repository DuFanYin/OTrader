#pragma once

/**
 * Strategy registry: class name to factory and parameters. Each strategy .cpp is its own library
 * whose REGISTER_STRATEGY runs as the program loads it.
 */

#include "strategy_api.hpp"
#include "model/parameters.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace strategy {

/** Hash of the headers strategies compile against, embedded in every strategy library; load()
 * reads it from the file before loading and refuses a mismatch. */
[[gnu::used]] inline constexpr char kStrategyApi[] = "optra-strategy-api:" OPTRA_STRATEGY_API;

class Strategy;
class StrategyContext;

/** Its Setting's parameters; empty when the class has none. */
template <typename Class> std::vector<model::Parameter> parameters_of_class() {
    if constexpr (requires { typename Class::Setting; }) {
        return model::parameters_of<typename Class::Setting>();
    } else {
        return {};
    }
}

/** One of a class, acting through `context` (bound to its record). */
using StrategyFactory = std::function<std::unique_ptr<Strategy>(StrategyContext& context)>;

class StrategyRegistry {
  public:
    /** A strategy library that was not loaded (or not all its classes), and why. */
    struct Refusal {
        std::string library;
        std::string why;
    };

    /** Returns whether the class was registered. A same-named class is replaced only by a new
     * build of the same library; another library's is kept and the newcomer refused (false). */
    static bool add(const std::string& class_name, StrategyFactory factory,
                    std::span<const model::Parameter> parameters, std::string source = {});

    /** Loads *.so in `directory` new or changed since last tried; remembers it for reload().
     * Never unloads: running strategies may use the old build, which a new one loads beside. */
    static void load(const std::filesystem::path& directory);
    /** load() again on every directory loaded before. */
    static void reload();
    /** <directory of the running program>/strategies. */
    static std::filesystem::path beside_executable();
    static std::vector<Refusal> refused();
    /** Empty for a class built into the program. Throws on an unknown class. */
    static std::string library(const std::string& class_name);
    static bool has(const std::string& class_name);
    /** Sorted. */
    static std::vector<std::string> get_all_strategy_class_names();
    /** The template's, then the class's. Throws on an unknown class. */
    static std::vector<model::Parameter> parameters(const std::string& class_name);
    /** Source file the class registered from; research results are keyed by it. Throws on an
     * unknown class. */
    static std::string source(const std::string& class_name);
    /** Defaults overridden by `setting` (model::resolve). Throws std::invalid_argument on an
     * unknown class or name, an out-of-range value or a fraction for an integer. */
    static model::Values resolve_setting(const std::string& class_name,
                                         const model::Values& setting);
    /** One of its record's class, acting through `context`; the record's setting resolved. Throws
     * on an unknown class. */
    static std::unique_ptr<Strategy> create(StrategyContext& context);
};

/** Use in a strategy's .cpp inside namespace strategy; parameters come from its Setting. */
#define REGISTER_STRATEGY(ClassName)                                                               \
    [[maybe_unused]] static const bool registered_##ClassName =                                    \
        ::strategy::StrategyRegistry::add(                                                         \
            #ClassName,                                                                            \
            [](::strategy::StrategyContext& context) -> std::unique_ptr<::strategy::Strategy> {    \
                return std::make_unique<ClassName>(context);                                       \
            },                                                                                     \
            ::strategy::parameters_of_class<ClassName>(), __FILE__)

} // namespace strategy
