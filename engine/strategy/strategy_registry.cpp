/**
 * Strategy registry. Strategies register themselves (REGISTER_STRATEGY in their own .cpp) during
 * static initialization; add_strategy errors on an unknown class or setting
 * (OptionStrategyEngine::add_strategy).
 */

#include "strategy_registry.hpp"
#include "template.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <mutex>
#include <ranges>
#include <stdexcept>

namespace strategy {

namespace {

struct Entry {
    StrategyFactory factory;
    std::vector<StrategyParam> parameters; // the template's, then the class's
};

struct Registry {
    std::unordered_map<std::string, Entry> classes;
    std::mutex mtx;
};
auto registry() -> Registry& {
    static Registry r;
    return r;
}

template <typename Range> auto join(const Range& names) -> std::string {
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

/** The class's entry; throws std::invalid_argument for an unknown class. */
auto entry_of(const std::string& class_name) -> Entry {
    {
        std::scoped_lock lock(registry().mtx);
        if (const auto it = registry().classes.find(class_name); it != registry().classes.end()) {
            return it->second;
        }
    }
    throw std::invalid_argument(
        std::format("Unknown strategy '{}'. Available: {}", class_name,
                    join(StrategyRegistry::get_all_strategy_class_names())));
}

} // namespace

auto StrategyRegistry::add(const std::string& class_name, StrategyFactory factory,
                           std::span<const StrategyParam> parameters) -> bool {
    Entry entry{.factory = std::move(factory), .parameters = {}};
    std::ranges::copy(OptionStrategyTemplate::base_parameters(),
                      std::back_inserter(entry.parameters));
    std::ranges::copy(parameters, std::back_inserter(entry.parameters));
    std::scoped_lock lock(registry().mtx);
    registry().classes[class_name] = std::move(entry);
    return true;
}

auto StrategyRegistry::has(const std::string& class_name) -> bool {
    std::scoped_lock lock(registry().mtx);
    return registry().classes.contains(class_name);
}

auto StrategyRegistry::get_all_strategy_class_names() -> std::vector<std::string> {
    std::scoped_lock lock(registry().mtx);
    std::vector<std::string> names;
    names.reserve(registry().classes.size());
    std::ranges::copy(registry().classes | std::views::keys, std::back_inserter(names));
    std::ranges::sort(names);
    return names;
}

auto StrategyRegistry::parameters(const std::string& class_name) -> std::vector<StrategyParam> {
    return entry_of(class_name).parameters;
}

auto StrategyRegistry::resolve_setting(const std::string& class_name,
                                       const std::unordered_map<std::string, double>& setting)
    -> std::unordered_map<std::string, double> {
    const std::vector<StrategyParam> declared = parameters(class_name);
    std::unordered_map<std::string, double> resolved;
    for (const StrategyParam& p : declared) {
        resolved.emplace(p.name, p.default_value);
    }
    for (const auto& [name, value] : setting) {
        auto it = resolved.find(name);
        if (it == resolved.end()) {
            throw std::invalid_argument(
                std::format("{} has no parameter '{}'; its parameters: {}", class_name, name,
                            join(declared | std::views::transform(&StrategyParam::name))));
        }
        it->second = value;
    }
    for (const StrategyParam& p : declared) {
        const double value = resolved.at(std::string(p.name));
        if (!(value >= p.min && value <= p.max)) { // NaN fails too
            throw std::invalid_argument(std::format("{}: {}={} is outside [{}, {}]", class_name,
                                                    p.name, value, p.min, p.max));
        }
        if (p.integer && value != std::floor(value)) {
            throw std::invalid_argument(
                std::format("{}: {}={} must be a whole number", class_name, p.name, value));
        }
    }
    return resolved;
}

auto StrategyRegistry::create(const std::string& class_name, StrategyContext* context,
                              const std::string& strategy_name, const std::string& portfolio_name,
                              const std::unordered_map<std::string, double>& setting)
    -> std::unique_ptr<OptionStrategyTemplate> {
    std::unique_ptr<OptionStrategyTemplate> strategy =
        entry_of(class_name).factory(context, strategy_name, portfolio_name, setting);
    strategy->set_class_name(class_name);
    return strategy;
}

} // namespace strategy
