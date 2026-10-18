#include "../kernel.h"

#include <algorithm>
#include <cmath>
#include <omp.h>

namespace quant {

void pricing_greeks(bool is_call, double spot, double strike, double rate, double dividend_yield,
                    double vol, double maturity, double* out_delta, double* out_gamma,
                    double* out_theta, double* out_vega, double* out_rho) {
    const double forward = spot_forward(spot, rate, dividend_yield, maturity);
    const BlackGreeks g = black_greeks(is_call, spot, forward, strike, maturity, rate, vol);
    const double price = black_price(is_call, forward, strike, maturity, rate, vol);
    // black_greeks holds the forward; with the spot held it drifts at rate - yield as time passes
    // and grows with the rate: theta = 365 theta_F - (r - q) S delta, rho = T (S delta - V).
    *out_delta = g.delta;
    *out_gamma = g.gamma;
    *out_vega  = 100.0 * g.vega;
    *out_theta = 365.0 * g.theta - (rate - dividend_yield) * spot * g.delta;
    *out_rho   = maturity * (spot * g.delta - price);
}

void greek_surface_grid(
    double strike, double rate, double vol, double dividend_yield,
    std::span<const double> spots,
    std::span<const double> maturities,
    int greek_code,
    std::span<double> out_grid
) {
    const int n_s = static_cast<int>(spots.size());
    const int n_t = static_cast<int>(maturities.size());
    if (n_s == 0 || n_t == 0 || static_cast<int>(out_grid.size()) < n_s * n_t) return;

#pragma omp parallel for schedule(static)
    for (int i = 0; i < n_s; ++i) {
        for (int j = 0; j < n_t; ++j) {
            double g[5];   // delta, gamma, vega, theta, rho: greek_code's order
            pricing_greeks(true, spots[i], strike, rate, dividend_yield, vol, maturities[j],
                           &g[0], &g[1], &g[3], &g[2], &g[4]);
            out_grid[i * n_t + j] = greek_code >= 0 && greek_code < 5 ? g[greek_code] : 0.0;
        }
    }
}

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

void pricing_batch(
    std::span<const double> spots,
    std::span<const double> strikes,
    std::span<const double> rates,
    std::span<const double> vols,
    std::span<const double> maturities,
    std::span<const int> n_paths_arr,
    std::span<const double> div_yields,
    std::span<double> out_mc,
    std::span<double> out_bs,
    std::span<double> out_binomial
) {
    const size_t n_jobs = spots.size();
    if (n_jobs == 0) return;
    if (strikes.size() != n_jobs || rates.size() != n_jobs || vols.size() != n_jobs ||
        maturities.size() != n_jobs || n_paths_arr.size() != n_jobs || div_yields.size() != n_jobs ||
        out_mc.size() != n_jobs || out_bs.size() != n_jobs || out_binomial.size() != n_jobs) return;

#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < static_cast<int>(n_jobs); ++i) {
        const double s = spots[i], k = strikes[i], r = rates[i], q = div_yields[i];
        const double v = vols[i], t = maturities[i];
        const int steps = std::max(10, static_cast<int>(t * 250));
        out_mc[i]       = mc_price(s, k, r, q, v, t, n_paths_arr[i], true);
        out_bs[i]       = black_price(true, spot_forward(s, r, q, t), k, t, r, v);
        out_binomial[i] = binomial_crr_price(s, k, r, q, v, t, steps, true, false);
    }
}

}  // namespace quant
