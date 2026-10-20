#include "../kernel.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#include <omp.h>

namespace quant {

namespace {

constexpr int PATH_CHUNK = 64;   // paths per random stream: results do not depend on the number of threads

// Black-Scholes value of one option on a spot that carries a continuous yield.
double bs_value(double s, double k, double r, double y, double vol, double tau, bool is_call) {
    return tau <= 0.0 ? payoff(s, k, is_call)
                      : black_price(is_call, spot_forward(s, r, y, tau), k, tau, r, vol);
}

// Its delta, as black_greeks works it out (the same operations, bit for bit), without pricing
// it: the hedge asks it of every leg at every step of every path.
double bs_delta(double s, double k, double r, double y, double vol, double tau, bool is_call) {
    if (tau <= 0.0) {
        const double in = is_call ? s - k : k - s;
        return in > 0.0 ? (is_call ? 1.0 : -1.0) : 0.0;
    }
    const double forward = spot_forward(s, r, y, tau);
    if (s <= 0.0 || forward <= 0.0 || k <= 0.0 || vol <= 0.0) {
        return 0.0;
    }
    const double sd = vol * std::sqrt(tau);
    const double d1 = (std::log(forward / k) / sd) + (0.5 * sd);
    const double per_spot = std::exp(-r * tau) * forward / s;
    return per_spot * (is_call ? norm_cdf(d1) : -norm_cdf(-d1));
}

}  // namespace

std::array<HedgeStrategy, 4> hedge_position(
    double spot, double rate, double dividend_yield, double path_vol,
    std::span<const HedgeLeg> legs, int n_rebalances, int n_paths,
    double transaction_cost_bps, double rebalance_threshold, double vol_mismatch_mult
) {
    const int steps = std::max(n_rebalances, 1);
    const int paths = std::max(n_paths, 1);
    double horizon = 0.0;
    for (const auto& leg : legs) horizon = std::max(horizon, leg.maturity);
    const double dt = clamp_positive(horizon) / steps, sqrt_dt = std::sqrt(dt);
    const double sigma = clamp_positive(path_vol);
    const double drift = (rate - dividend_yield - 0.5 * sigma * sigma) * dt;
    const double growth = std::exp(rate * dt);
    const double fee = clamp_nonnegative(transaction_cost_bps) * 1e-4;
    const double threshold = clamp_nonnegative(rebalance_threshold);
    const double mismatch = clamp_positive(vol_mismatch_mult, 0.1);

    // The four strategies, in the order of kHedgeStrategies.
    struct Plan { double fee; bool on_threshold; bool mismatched; };
    const Plan plans[4] = {{0.0, false, false}, {fee, false, false}, {fee, true, false}, {fee, false, true}};

    // Each leg settles in cash at the step nearest its expiry.
    std::vector<int> expiry_step(legs.size());
    for (size_t i = 0; i < legs.size(); ++i)
        expiry_step[i] = std::clamp(static_cast<int>(std::lround(legs[i].maturity / dt)), 1, steps);

    // The position's delta at step j from the legs still alive then, at vol × mult (the hedge holds minus this).
    auto position_delta = [&](double s, int j, double mult) {
        double d = 0.0;
        for (size_t i = 0; i < legs.size(); ++i) {
            if (j >= expiry_step[i] || legs[i].quantity == 0.0) continue;
            const double tau = std::max(legs[i].maturity - j * dt, 1e-10);
            d += legs[i].quantity * bs_delta(s, legs[i].strike, rate, legs[i].yield,
                                             legs[i].vol * mult, tau, legs[i].is_call);
        }
        return d;
    };
    double position_cost = 0.0;
    for (const auto& leg : legs)
        position_cost += leg.quantity * bs_value(spot, leg.strike, rate, leg.yield, leg.vol,
                                                 leg.maturity, leg.is_call);

    std::array<HedgeStrategy, 4> out;
    for (int st = 0; st < 4; ++st) {
        out[st].name = kHedgeStrategies[st];
        out[st].pnl.assign(paths, 0.0);
        out[st].turnover.assign(paths, 0.0);
        out[st].cost.assign(paths, 0.0);
    }

    const int chunks = (paths + PATH_CHUNK - 1) / PATH_CHUNK;
    #pragma omp parallel for schedule(dynamic)
    for (int c = 0; c < chunks; ++c) {
        std::mt19937_64 rng(777 + static_cast<unsigned>(c));
        std::normal_distribution<double> z(0.0, 1.0);
        for (int k = c * PATH_CHUNK; k < std::min(paths, (c + 1) * PATH_CHUNK); ++k) {
            double s = spot;
            double shares[4] = {}, cash[4], traded[4] = {}, fees[4] = {};
            auto trade_to = [&](int st, double target) {
                const double amount = target - shares[st], paid = std::abs(amount) * s * plans[st].fee;
                cash[st] -= amount * s + paid;
                traded[st] += std::abs(amount);
                fees[st] += paid;
                shares[st] = target;
            };
            const double delta0[2] = {position_delta(s, 0, 1.0), position_delta(s, 0, mismatch)};
            for (int st = 0; st < 4; ++st) {
                cash[st] = -position_cost;
                trade_to(st, -delta0[plans[st].mismatched]);
            }
            for (int j = 1; j <= steps; ++j) {
                s *= std::exp(drift + sigma * sqrt_dt * z(rng));
                double settled = 0.0;
                bool alive = false;
                for (size_t i = 0; i < legs.size(); ++i) {
                    if (expiry_step[i] == j)
                        settled += legs[i].quantity * payoff(s, legs[i].strike, legs[i].is_call);
                    alive |= expiry_step[i] > j;
                }
                // once every leg has expired the target is no shares at all
                const double delta[2] = {alive ? position_delta(s, j, 1.0) : 0.0, alive ? position_delta(s, j, mismatch) : 0.0};
                for (int st = 0; st < 4; ++st) {
                    cash[st] = cash[st] * growth + shares[st] * s * dividend_yield * dt + settled;
                    const double target = -delta[plans[st].mismatched];
                    if (plans[st].on_threshold && alive && std::abs(target - shares[st]) <= threshold) continue;
                    trade_to(st, target);
                }
            }
            for (int st = 0; st < 4; ++st) {
                out[st].pnl[k]      = cash[st] + shares[st] * s;
                out[st].turnover[k] = traded[st];
                out[st].cost[k]     = fees[st];
            }
        }
    }
    return out;
}

PnlStats hedge_stats(const HedgeStrategy& run) {
    PnlStats st;
    if (run.pnl.empty()) return st;
    std::vector<double> sorted = run.pnl;
    std::ranges::sort(sorted);
    st.q05 = interp_quantile_sorted(sorted, 0.05);
    st.q50 = interp_quantile_sorted(sorted, 0.50);
    st.q95 = interp_quantile_sorted(sorted, 0.95);
    double tail = 0.0;
    int in_tail = 0;
    for (double x : sorted) {
        if (x > st.q05) break;
        tail += x;
        ++in_tail;
    }
    st.mean     = sample_mean(sorted);
    st.std      = std::sqrt(clamp_nonnegative(sample_variance(sorted)));
    st.turnover = run.turnover.empty() ? 0.0 : sample_mean(run.turnover);
    st.cost     = run.cost.empty() ? 0.0 : sample_mean(run.cost);
    st.var95    = -st.q05;
    st.es95     = in_tail > 0 ? -(tail / in_tail) : 0.0;
    return st;
}

void pnl_histogram(const std::vector<double>& pnl, int n_bins, std::span<double> edges, std::span<double> counts) {
    if (n_bins < 1 || pnl.empty() || edges.size() < static_cast<size_t>(n_bins + 1) || counts.size() < static_cast<size_t>(n_bins))
        return;
    const auto [lo_it, hi_it] = std::ranges::minmax_element(pnl);
    const double lo = *lo_it, width = std::max(*hi_it - lo, 1e-15);
    for (int b = 0; b <= n_bins; ++b) edges[b] = lo + width * b / n_bins;
    std::ranges::fill(counts.first(n_bins), 0.0);
    for (double v : pnl) counts[std::min(static_cast<int>((v - lo) / width * n_bins), n_bins - 1)] += 1.0;
}

}  // namespace quant
