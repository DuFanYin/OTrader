#include "engine.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace quant::analytics {

ImpliedVolResult run_implied_vol(const ImpliedVolParams& p) {
    ImpliedVolResult r;
    const double iv = bs_implied_vol(
        p.market_price, p.spot, p.strike, p.rate, p.maturity, p.dividend_yield);
    r.implied_vol = iv;
    r.converged   = (iv > 0.0);
    if (r.converged) {
        const double r_eff = p.rate - p.dividend_yield;
        const double bs = bs_closed_form_price(p.spot, p.strike, r_eff, iv, p.maturity);
        r.final_error = std::abs(bs - p.market_price);
    }
    return r;
}

ImpliedVolBatchResult run_implied_vol_batch(const ImpliedVolBatchParams& p) {
    const int n = static_cast<int>(p.market_prices.size());
    ImpliedVolBatchResult r;
    r.ivs.resize(n);
    r.converged.resize(n);
    implied_vol_batch(
        std::span<const double>(p.market_prices),
        std::span<const double>(p.strikes),
        std::span<const double>(p.expiries),
        p.spot, p.rate, p.dividend_yield,
        std::span<double>(r.ivs),
        std::span<uint8_t>(r.converged)
    );
    r.n_converged = static_cast<int>(
        std::count(r.converged.begin(), r.converged.end(), true));
    return r;
}

HestonCalibrationResult run_heston_calibrate(const HestonCalibrationParams& p) {
    const int n = static_cast<int>(p.market_prices.size());
    HestonCalibrationResult r;
    if (n == 0) return r;

    // The fit is quant's, the one the trading engine's strategies use
    const HestonFit fit = heston_calibrate(
        p.spot, p.rate, p.market_strikes, p.market_maturities, p.market_prices,
        {p.init_v0, p.init_kappa, p.init_theta, p.init_xi, p.init_rho}, p.max_iter);
    r.v0    = fit.params.v0;
    r.kappa = fit.params.kappa;
    r.theta = fit.params.theta;
    r.xi    = fit.params.xi;
    r.rho   = fit.params.rho;
    r.iterations = fit.iterations;
    r.converged  = fit.converged;

    // Compute residuals and diagnostics
    r.model_prices.resize(n);
    r.residuals.resize(n);
    double max_err = 0.0;
    for (int i = 0; i < n; ++i) {
        r.model_prices[i] = heston_call_price(
            p.spot, p.market_strikes[i], p.rate, p.market_maturities[i],
            r.v0, r.kappa, r.theta, r.xi, r.rho);
        r.residuals[i] = r.model_prices[i] - p.market_prices[i];
        max_err = std::max(max_err, std::abs(r.residuals[i]));
    }
    const double mse = std::accumulate(r.residuals.begin(), r.residuals.end(), 0.0,
        [](double acc, double e) { return acc + e * e; }) / n;
    r.rmse          = std::sqrt(mse);
    r.max_abs_error = max_err;
    return r;
}

double run_heston_price(const HestonPriceParams& p) {
    return heston_call_price(
        p.spot, p.strike, p.rate, p.maturity,
        p.v0, p.kappa, p.theta, p.xi, p.rho);
}

}  // namespace quant::analytics
