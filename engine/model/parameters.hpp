#pragma once

/**
 * Named numbers set by name, each with a default and a range: a strategy's setting, the risk
 * limits. Declared as a struct of Params:
 *
 *     struct Setting {
 *         Param<int> lookback{"lookback", 60, 2, 390};
 *         Param<double> threshold{"threshold", 2.0, 0.0, 10.0};
 *     };
 *
 * parameters_of lists them, resolve checks values against them, read fills one in. Searches vary
 * them within their ranges. A derived struct lists its base's parameters first.
 */

#include <concepts>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace model {

/** A declared parameter: its name, default and range. */
struct Parameter {
    std::string_view name;
    double default_value = 0.0;
    double min = 0.0;
    double max = 0.0;
    bool integer = false;
};

/** Values by name. */
using Values = std::unordered_map<std::string, double>;

/** Every parameter at its default, overridden by `values`. Throws std::invalid_argument for a name
 * not declared, a value out of range or a fraction for an integer; `owner` begins the message. */
Values resolve(std::span<const Parameter> parameters, const Values& values, std::string_view owner);

namespace detail {

/** A declared Param and how to assign its value. */
struct DeclaredParam {
    Parameter param;
    void* value = nullptr;
    void (*assign)(void* value, double to) = nullptr;
};

/** Non-null only while a struct of Params is built: Params register here in construction order. */
inline thread_local std::vector<DeclaredParam>* declaring = nullptr;

/** RAII scope collecting Params declared during its lifetime; nests. */
class Declaring {
  public:
    Declaring() : outer_(declaring) { declaring = &params; }
    ~Declaring() { declaring = outer_; }
    Declaring(const Declaring&) = delete;
    Declaring& operator=(const Declaring&) = delete;

    std::vector<DeclaredParam> params;

  private:
    std::vector<DeclaredParam>* outer_;
};

} // namespace detail

/** A member of a struct of parameters: holds the default until read fills it; converts to T. */
template <typename T>
    requires std::same_as<T, int> || std::same_as<T, double>
class Param {
  public:
    Param(std::string_view name, T default_value, T min, T max) : value_(default_value) {
        if (detail::declaring != nullptr) {
            detail::declaring->push_back(
                {.param = {.name = name,
                           .default_value = static_cast<double>(default_value),
                           .min = static_cast<double>(min),
                           .max = static_cast<double>(max),
                           .integer = std::is_integral_v<T>},
                 .value = &value_,
                 .assign = [](void* value, double to) {
                     *static_cast<T*>(value) = static_cast<T>(to);
                 }});
        }
    }
    Param(const Param&) = default; // a copy declares nothing
    Param& operator=(const Param&) = default;

    operator T() const { return value_; }
    [[nodiscard]] T value() const { return value_; }

  private:
    T value_;
};

template <typename Params> std::vector<Parameter> parameters_of() {
    detail::Declaring declaring;
    [[maybe_unused]] const Params params;
    std::vector<Parameter> out;
    out.reserve(declaring.params.size());
    for (const detail::DeclaredParam& d : declaring.params) {
        out.push_back(d.param);
    }
    return out;
}

/** The struct with each parameter's value from `resolved` (resolve's: every one there). Throws
 * std::logic_error for one missing. */
template <typename Params> Params read(const Values& resolved) {
    detail::Declaring declaring;
    Params params;
    for (const detail::DeclaredParam& d : declaring.params) {
        const auto it = resolved.find(std::string(d.param.name));
        if (it == resolved.end()) {
            throw std::logic_error(std::format("No value for parameter '{}'", d.param.name));
        }
        d.assign(d.value, it->second);
    }
    return params;
}

} // namespace model

template <typename T> struct std::formatter<model::Param<T>> : std::formatter<T> {
    auto format(const model::Param<T>& p, std::format_context& ctx) const {
        return std::formatter<T>::format(p.value(), ctx);
    }
};
