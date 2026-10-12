#include "../kernel.h"
#include <cmath>
#include <omp.h>

namespace quant {

// The vol at which bs_closed_form_price (at r - q: its drift and its discount) gives market_price: Black on the
// forward S e^{(r-q)T}, discounted at r - q, solved by LetsBeRational. -1.0 when there is none in [1e-4, 5].
double bs_implied_vol(
    double market_price, double spot, double strike,
    double rate, double maturity, double dividend_yield
) {
    if (market_price <= 0.0 || spot <= 0.0 || strike <= 0.0 || maturity <= 0.0)
        return -1.0;
    const double r_eff = rate - dividend_yield;
    const double forward = spot * std::exp(r_eff * maturity);
    const double iv = black_implied_vol(market_price, forward, strike, maturity, r_eff, true);
    return (iv >= 1e-4 && iv < 5.0) ? iv : -1.0;
}

void implied_vol_batch(
    std::span<const double>  market_prices,
    std::span<const double>  strikes,
    std::span<const double>  expiries,
    double spot, double rate, double dividend_yield,
    std::span<double>        out_ivs,
    std::span<uint8_t>       out_converged
) {
    const int n = static_cast<int>(market_prices.size());
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        const double iv = bs_implied_vol(
            market_prices[i], spot, strikes[i], rate, expiries[i], dividend_yield);
        out_ivs[i]       = iv;
        out_converged[i] = static_cast<uint8_t>(iv > 0.0 ? 1 : 0);
    }
}

}  // namespace quant
