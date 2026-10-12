#include "engine.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <vector>

namespace quant::analytics {

ScenarioResult run_scenario(const ScenarioParams& p) {
    double prices[5] = {0};
    scenario_bs5(p.spot, p.strike, p.rate, p.vol, p.maturity, p.dividend_yield, p.option_type != "put", prices);

    static const char* names[5] = {"base", "spot_up", "spot_down", "vol_up", "rate_up"};

    // rank by abs deviation from base
    int idx[5] = {0, 1, 2, 3, 4};
    std::ranges::sort(idx, [&](int a, int b) {
        return std::abs(prices[a] - prices[0]) > std::abs(prices[b] - prices[0]);
    });
    int ranks[5] = {};
    for (int i = 0; i < 5; ++i) ranks[idx[i]] = i + 1;

    ScenarioResult r;
    r.base_price = prices[0];
    r.max_price  = *std::ranges::max_element(prices);
    r.min_price  = *std::ranges::min_element(prices);

    for (int i = 0; i < 5; ++i) {
        ScenarioRow row;
        row.name         = names[i];
        row.bs_price     = prices[i];
        row.vs_base_diff = prices[i] - prices[0];
        row.abs_diff     = std::abs(row.vs_base_diff);
        row.rank         = ranks[i];
        row.has_pct      = std::abs(prices[0]) > 1e-12;
        if (row.has_pct) {
            row.vs_base_pct = row.vs_base_diff / prices[0];
            row.vs_base_bps = row.vs_base_pct * 10000.0;
        }
        r.rows.push_back(row);
    }
    return r;
}

}  // namespace quant::analytics
