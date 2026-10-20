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

}  // namespace quant
