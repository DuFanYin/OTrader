#include "../kernel.h"
#include "lets_be_rational_api.hpp"
#include <algorithm>
#include <cmath>

namespace quant {

namespace {

constexpr double kMaxVol = 5.0;

} // namespace

BlackGreeks black_greeks(bool is_call, double spot, double forward, double strike, double maturity,
                         double rate, double vol) {
    BlackGreeks g;
    if (spot <= 0.0 || forward <= 0.0 || strike <= 0.0 || maturity <= 0.0 || vol <= 0.0) {
        return g;
    }
    const double t = maturity;
    const double sqrt_t = std::sqrt(t);
    const double sd = vol * sqrt_t;
    const double d1 = (std::log(forward / strike) / sd) + (0.5 * sd);
    const double d2 = d1 - sd;
    const double pdf = norm_pdf(d1);
    const double df = std::exp(-rate * t);
    const double per_spot = df * forward / spot; // what the discounted forward moves per unit of spot

    g.delta = per_spot * (is_call ? norm_cdf(d1) : norm_cdf(d1) - 1.0);
    g.gamma = per_spot * pdf / (spot * sd);
    const double price = black_price(is_call, forward, strike, t, rate, vol);
    g.theta = ((rate * price) - (df * forward * pdf * vol / (2.0 * sqrt_t))) / 365.0;
    g.vega = df * forward * pdf * sqrt_t / 100.0;
    return g;
}

double black_price(bool is_call, double forward, double strike, double maturity, double rate,
                   double vol) {
    if (forward <= 0.0 || strike <= 0.0 || maturity <= 0.0 || vol <= 0.0) {
        return 0.0;
    }
    const double sd = vol * std::sqrt(maturity);
    const double d1 = (std::log(forward / strike) / sd) + (0.5 * sd);
    const double d2 = d1 - sd;
    const double df = std::exp(-rate * maturity);
    return is_call ? df * ((forward * norm_cdf(d1)) - (strike * norm_cdf(d2)))
                   : df * ((strike * norm_cdf(-d2)) - (forward * norm_cdf(-d1)));
}

double black_implied_vol(double price, double forward, double strike, double maturity, double rate,
                         bool is_call) {
    if (price <= 0.0 || forward <= 0.0 || strike <= 0.0 || maturity <= 0.0) {
        return 0.0;
    }
    const double q = is_call ? 1.0 : -1.0;
    const double undiscounted = price * std::exp(rate * maturity);
    const double iv = ImpliedBlackVolatility(undiscounted, forward, strike, maturity, q);
    if (!std::isfinite(iv) || iv <= 0.0) {
        return 0.0;
    }
    return std::min(iv, kMaxVol);
}

double black_digital_call(double forward, double strike, double maturity, double rate, double vol) {
    if (forward <= 0.0 || strike <= 0.0) {
        return 0.0;
    }
    const double df = std::exp(-rate * std::max(maturity, 0.0));
    if (maturity <= 0.0 || vol <= 0.0) {
        return forward > strike ? df : 0.0;
    }
    const double sd = vol * std::sqrt(maturity);
    return df * norm_cdf((std::log(forward / strike) / sd) - (0.5 * sd));
}

} // namespace quant
