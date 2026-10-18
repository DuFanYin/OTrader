// Hedging task: a position delta-hedged on simulated paths, by each way of hedging, compared.
#include "json_io.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

namespace quant::analytics {

std::string run_hedging_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "strike", "rate", "vol", "maturity"},
                                   "missing_required_hedging_fields");
    if (!req) return req.error();
    const json& j = *req;
    const double spot = jv(j, "spot", 0.0), strike = jv(j, "strike", 0.0), rate = jv(j, "rate", 0.0);
    const double vol = jv(j, "vol", 0.0), maturity = jv(j, "maturity", 0.0), q = jv(j, "dividend_yield", 0.0);
    const int n_rebalances = jv(j, "n_rebalances", 52), n_paths = jv(j, "n_paths", 2000);
    const double tc_bps = jv(j, "transaction_cost_bps", 5.0), threshold = jv(j, "rebalance_threshold", 0.02);
    const double mismatch = jv(j, "vol_mismatch_mult", 1.15);

    // The position: the legs given, or one option sold at the strike (a market maker's hedge).
    std::vector<HedgeLeg> legs;
    for (const auto& leg : parse_legs(j, spot)) {
        const double t = leg.maturity > 0.0 ? leg.maturity : maturity;
        // A leg on its own forward F carries ln(F/S)/T: as a yield on the spot, r minus that.
        const double y = leg.forward > 0.0 ? rate - std::log(leg.forward / spot) / t : q;
        legs.push_back({leg.option_type != "put", leg.strike, leg.quantity, leg.vol > 0.0 ? leg.vol : vol, t, y});
    }
    if (legs.empty())
        legs.push_back({jv<std::string>(j, "option_type", "call") != "put", strike, -1.0, vol, maturity, q});

    const auto runs = hedge_position(spot, rate, q, vol, legs, n_rebalances, n_paths, tc_bps, threshold, mismatch);
    json compare = json::object();
    std::vector<std::pair<std::string, PnlStats>> stats;
    for (const auto& run : runs) {
        const PnlStats st = hedge_stats(run);
        stats.emplace_back(run.name, st);
        compare[run.name] = {{"mean", st.mean}, {"std", st.std}, {"q05", st.q05}, {"q50", st.q50},
                             {"q95", st.q95}, {"turnover", st.turnover}, {"transaction_cost", st.cost},
                             {"var95", st.var95}, {"es95", st.es95}};
    }
    // The best: the lowest ES95, then the lowest std, then the highest mean.
    const auto& [best_name, best] = std::ranges::min(
        stats, {}, [](const auto& s) { return std::tuple(s.second.es95, s.second.std, -s.second.mean); });

    // The headline numbers and the histogram are discrete delta hedging's, on the same paths as the comparison.
    const PnlStats& base = stats.front().second;
    const int n_bins = std::max(jv(j, "n_bins", 40), 10);
    std::vector<double> edges(n_bins + 1, 0.0), counts(n_bins, 0.0);
    pnl_histogram(runs.front().pnl, n_bins, edges, counts);
    double horizon = 0.0;
    for (const auto& leg : legs) horizon = std::max(horizon, leg.maturity);
    const double left = base.q05 - base.mean, right = base.q95 - base.mean;
    const double tails = std::abs(right) + std::abs(left);
    json summary = {
        {"strategy", "delta_hedge"},
        {"n_rebalances", n_rebalances},
        {"n_paths", n_paths},
        {"pnl_mean", base.mean},
        {"pnl_std", base.std},
        {"pnl_q05", base.q05},
        {"pnl_q50", base.q50},
        {"pnl_q95", base.q95},
        {"normalized_std", base.std / std::max(spot, 1e-12)},
        {"tail_span", base.q95 - base.q05},
        {"left_tail", left},
        {"right_tail", right},
        {"tail_skew_proxy", tails > 1e-12 ? (right + left) / tails : 0.0},
        {"tail_ratio_q95_q05", std::abs(base.q05) > 1e-12 ? std::abs(base.q95 / base.q05) : 0.0},
        {"scaling_proxy_std_sqrt_n", base.std * std::sqrt(static_cast<double>(n_rebalances))},
        {"best_strategy", best_name},
        {"n_legs", static_cast<int>(legs.size())},
        {"horizon", horizon},
    };
    json details = {
        {"histogram", {{"edges", edges}, {"counts", counts}}},
        {"strategy_compare", std::move(compare)},
        {"best_strategy", {{"name", best_name}, {"reason", "min_es95_then_std_then_mean"}, {"es95", best.es95},
                           {"std", best.std}, {"mean", best.mean}}},
        {"compare_config",
         {{"transaction_cost_bps", tc_bps}, {"rebalance_threshold", threshold}, {"vol_mismatch_mult", mismatch}}},
    };
    return ok_response(std::move(summary), std::move(details), t0_ms);
}

}  // namespace quant::analytics
