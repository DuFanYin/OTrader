#include "engine.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

namespace quant::analytics {

namespace {

// The position to hedge: the legs given, or one option sold at the strike (a market maker's hedge).
std::vector<HedgeLeg> position_legs(const HedgingParams& p) {
    if (p.legs.empty()) return {{p.option_type != "put", p.strike, -1.0, p.vol, p.maturity, p.dividend_yield}};
    std::vector<HedgeLeg> legs;
    for (const auto& leg : p.legs) {
        const double t = leg.maturity > 0.0 ? leg.maturity : p.maturity;
        // A leg on its own forward F carries ln(F/S)/T: as a yield on the spot, r minus that.
        const double y = leg.forward > 0.0 ? p.rate - std::log(leg.forward / p.spot) / t : p.dividend_yield;
        legs.push_back({leg.option_type != "put", leg.strike, leg.quantity, leg.vol > 0.0 ? leg.vol : p.vol, t, y});
    }
    return legs;
}

}  // namespace

HedgingResult run_hedging(const HedgingParams& p) {
    const auto legs = position_legs(p);
    const auto runs = hedge_position(p.spot, p.rate, p.dividend_yield, p.vol, legs, p.n_rebalances, p.n_paths,
                                     p.transaction_cost_bps, p.rebalance_threshold, p.vol_mismatch_mult);
    HedgingResult r;
    for (const auto& run : runs) {
        const PnlStats st = hedge_stats(run);
        r.strategies.push_back({run.name, st.mean, st.std, st.q05, st.q50, st.q95, st.turnover, st.cost, st.var95, st.es95});
    }
    // The best: the lowest ES95, then the lowest std, then the highest mean.
    const auto rank = [](const StrategyStats& s) { return std::tuple(s.es95, s.std, -s.mean); };
    r.best_strategy = std::ranges::min(r.strategies, {}, rank).name;

    // The headline numbers and the histogram are discrete delta hedging's, on the same paths as the comparison.
    const StrategyStats& base = r.strategies.front();
    r.pnl_mean = base.mean;
    r.pnl_std  = base.std;
    r.pnl_q05  = base.q05;
    r.pnl_q50  = base.q50;
    r.pnl_q95  = base.q95;
    const int n_bins = std::max(p.n_bins, 10);
    r.histogram.edges.resize(n_bins + 1, 0.0);
    r.histogram.counts.resize(n_bins, 0.0);
    pnl_histogram(runs.front().pnl, n_bins, r.histogram.edges, r.histogram.counts);

    r.n_rebalances = p.n_rebalances;
    r.n_paths      = p.n_paths;
    r.n_legs       = static_cast<int>(legs.size());
    for (const auto& leg : legs) r.horizon = std::max(r.horizon, leg.maturity);
    r.normalized_std   = r.pnl_std / std::max(p.spot, 1e-12);
    r.tail_span        = r.pnl_q95 - r.pnl_q05;
    r.left_tail        = r.pnl_q05 - r.pnl_mean;
    r.right_tail       = r.pnl_q95 - r.pnl_mean;
    r.tail_skew_proxy  = (std::abs(r.right_tail) + std::abs(r.left_tail)) > 1e-12
                         ? (r.right_tail + r.left_tail) / (std::abs(r.right_tail) + std::abs(r.left_tail))
                         : 0.0;
    r.tail_ratio_q95_q05 = std::abs(r.pnl_q05) > 1e-12
                           ? std::abs(r.pnl_q95 / r.pnl_q05) : 0.0;
    r.scaling_proxy_std_sqrt_n = r.pnl_std * std::sqrt(static_cast<double>(p.n_rebalances));
    r.compare_tc_bps             = p.transaction_cost_bps;
    r.compare_threshold          = p.rebalance_threshold;
    r.compare_vol_mismatch_mult  = p.vol_mismatch_mult;
    return r;
}

}  // namespace quant::analytics
