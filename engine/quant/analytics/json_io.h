#pragma once
// Reading a task's request and writing its answer in the one shape (engine.h); shared by the
// workflow files, not part of the library's interface.

#include "engine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <expected>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace quant::analytics {

using json = nlohmann::json;

inline double elapsed_ms(double t0_ms) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(now).count() - t0_ms;
}

// The field, or `def` when it is absent, null or of another type.
template <typename T>
T jv(const json& j, std::string_view k, T def) {
    const std::string key(k);
    if (j.contains(key) && !j[key].is_null()) {
        try {
            return j[key].get<T>();
        } catch (...) {
        }
    }
    return def;
}

inline std::string ok_response(json summary, json details, double t0_ms) {
    return json{{"contract_version", kContractVersion}, {"status", "ok"},
                {"result_summary", std::move(summary)}, {"result_details", std::move(details)},
                {"runtime_ms", elapsed_ms(t0_ms)}, {"error", nullptr}}
        .dump();
}

inline std::string err_response(const std::string& message) {
    return json{{"contract_version", kContractVersion}, {"status", "error"},
                {"result_summary", json::object()}, {"result_details", json::object()},
                {"runtime_ms", nullptr},
                {"error", {{"code", "bad_request"}, {"message", message}}}}
        .dump();
}

inline bool has_fields(const json& j, std::initializer_list<std::string_view> keys) {
    return std::ranges::all_of(keys, [&](std::string_view key) { return j.contains(std::string(key)); });
}

// The request, or the error answer: not JSON, or without a field of `required`.
inline std::expected<json, std::string> parse_request(const std::string& src,
                                                      std::initializer_list<std::string_view> required = {},
                                                      std::string_view missing_error = "") {
    json body;
    try {
        body = json::parse(src);
    } catch (...) {
        return std::unexpected(err_response("invalid_json"));
    }
    if (!has_fields(body, required)) return std::unexpected(err_response(std::string(missing_error)));
    return body;
}

// A whole number as an integer (a count proposed by the validation gate reads 4000, not 4000.0).
inline json number(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 9.0e15) return static_cast<long long>(v);
    return v;
}

// One leg of a request's position (multi_leg, hedging).
struct LegSpec {
    std::string option_type = "call";  // "call" | "put"
    double strike = 0.0;
    double quantity = 1.0;  // signed: positive long, negative short
    // Optional per-leg values; <= 0 means the request's own.
    double vol = 0.0;
    double maturity = 0.0;
    double forward = 0.0;  // price this leg off its own forward (Black-76 equivalent)
};

inline std::vector<LegSpec> parse_legs(const json& j, double default_strike) {
    std::vector<LegSpec> legs;
    if (!j.contains("legs") || !j["legs"].is_array()) return legs;
    for (const auto& leg : j["legs"]) {
        legs.push_back({jv<std::string>(leg, "option_type", "call"), jv(leg, "strike", default_strike),
                        jv(leg, "quantity", 1.0), jv(leg, "vol", 0.0), jv(leg, "maturity", 0.0),
                        jv(leg, "forward", 0.0)});
    }
    return legs;
}

}  // namespace quant::analytics
