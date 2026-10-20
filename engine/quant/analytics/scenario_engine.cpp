// Scenario task: one option repriced under four shocks (spot up and down, vol up, rate up),
// ranked by how far each moves it.
#include "json_io.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace quant {

void scenario_bs5(
    double spot, double strike, double rate, double vol,
    double maturity, double dividend_yield, bool is_call, double* out_prices_5
) {
    if (!out_prices_5) return;
    const double shock_spot[5]  = {1.0, 1.1, 0.9, 1.0, 1.0};
    const double shock_vol[5]   = {0.0, 0.0, 0.0, 0.05, 0.0};
    const double shock_rate[5]  = {0.0, 0.0, 0.0, 0.0, 0.01};
    for (int i = 0; i < 5; ++i) {
        const double s = spot * shock_spot[i];
        const double v = std::max(0.0001, vol + shock_vol[i]);
        const double r = rate + shock_rate[i];
        out_prices_5[i] = black_price(is_call, spot_forward(s, r, dividend_yield, maturity), strike,
                                      maturity, r, v);
    }
}

}  // namespace quant

namespace quant::analytics {

std::string run_scenario_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                   "missing_required_scenario_fields");
    if (!req) return req.error();
    const json& j = *req;
    double prices[5] = {0};
    scenario_bs5(jv(j, "spot", 0.0), jv(j, "strike", 0.0), jv(j, "rate", 0.0), jv(j, "vol", 0.0),
                 jv(j, "maturity", 0.0), jv(j, "dividend_yield", 0.0),
                 jv<std::string>(j, "option_type", "call") != "put", prices);
    static const char* names[5] = {"base", "spot_up", "spot_down", "vol_up", "rate_up"};

    // Ranked by the distance from the base price.
    int idx[5] = {0, 1, 2, 3, 4};
    std::ranges::sort(idx, [&](int a, int b) {
        return std::abs(prices[a] - prices[0]) > std::abs(prices[b] - prices[0]);
    });
    int ranks[5] = {};
    for (int i = 0; i < 5; ++i) ranks[idx[i]] = i + 1;

    const double base = prices[0];
    const bool has_pct = std::abs(base) > 1e-12;
    json rows = json::array(), tornado = json::array();
    for (int i = 0; i < 5; ++i) {
        const double diff = prices[i] - base;
        const double pct = has_pct ? diff / base : 0.0;
        rows.push_back({{"rank", ranks[i]}, {"scenario", names[i]}, {"base_price", base},
                        {"bs_price", prices[i]}, {"vs_base_diff", diff}, {"abs_vs_base_diff", std::abs(diff)},
                        {"vs_base_pct", has_pct ? json(pct) : json(nullptr)},
                        {"vs_base_bps", has_pct ? json(pct * 10000.0) : json(nullptr)}});
        if (i > 0) tornado.push_back({{"label", names[i]}, {"value", diff}});
    }
    const double max_price = *std::ranges::max_element(prices);
    const double min_price = *std::ranges::min_element(prices);
    json summary = {{"base_price", base}, {"max_price", max_price}, {"min_price", min_price},
                    {"price_range", max_price - min_price}, {"scenario_count", 5}};
    return ok_response(std::move(summary), {{"rows", std::move(rows)}, {"tornado_points", std::move(tornado)}},
                       t0_ms);
}

}  // namespace quant::analytics
