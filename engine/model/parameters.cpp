#include "model/parameters.hpp"
#include "utilities/text.hpp"
#include <cmath>
#include <ranges>

namespace model {

auto resolve(std::span<const Parameter> parameters, const Values& values, std::string_view owner)
    -> Values {
    Values resolved;
    for (const Parameter& p : parameters) {
        resolved.emplace(p.name, p.default_value);
    }
    for (const auto& [name, value] : values) {
        auto it = resolved.find(name);
        if (it == resolved.end()) {
            throw std::invalid_argument(
                std::format("{} has no parameter '{}'; its parameters: {}", owner, name,
                            utilities::join(parameters | std::views::transform(&Parameter::name))));
        }
        it->second = value;
    }
    for (const Parameter& p : parameters) {
        const double value = resolved.at(std::string(p.name));
        if (!(value >= p.min && value <= p.max)) { // NaN fails too
            throw std::invalid_argument(std::format("{}: {}={} is outside [{}, {}]", owner,
                                                    p.name, value, p.min, p.max));
        }
        if (p.integer && value != std::floor(value)) {
            throw std::invalid_argument(
                std::format("{}: {}={} must be a whole number", owner, p.name, value));
        }
    }
    return resolved;
}

} // namespace model
