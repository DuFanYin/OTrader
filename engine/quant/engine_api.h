#pragma once

/** The numerics the engine and its strategies call: Black's price, Greeks and implied vol, the
 * features of a strategy's values by day, SVI's smile and fit. Part of the strategy API (its hash):
 * what changes here makes every strategy library build again. The rest is kernel.h's. */

#include <cmath>
#include <span>

namespace quant {

// Every European closed form is Black on the forward, discounted at `rate`: the trading engine's
// Greeks and implied vol, and Black-Scholes on a spot paying a yield, as Black on spot_forward.
/** Per-unit Greeks; theta per day, vega per 1% vol move (as py_vollib). */
struct BlackGreeks {
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double vega = 0.0;
};
/** Greeks of an option priced as Black on `forward`, discounted at `rate`. Delta and gamma are to
 * `spot`, the forward moving with it (in proportion); theta holds the forward. */
BlackGreeks black_greeks(bool is_call, double spot, double forward, double strike, double maturity,
                         double rate, double vol);
/** Black on `forward`, discounted at `rate`: the option's price; 0 for a non-positive input. */
double black_price(bool is_call, double forward, double strike, double maturity, double rate,
                   double vol);
/** The vol at which Black on `forward`, discounted at `rate`, gives `price` (LetsBeRational, to
 * machine precision); 0 when there is none, at most 5. */
double black_implied_vol(double price, double forward, double strike, double maturity, double rate,
                         bool is_call);

/** The forward of a spot paying a continuous `dividend_yield`. */
inline double spot_forward(double spot, double rate, double dividend_yield, double maturity) {
    return spot * std::exp((rate - dividend_yield) * maturity);
}

// Features of a strategy's data (strategy::Strategy wraps them). The annualised standard deviation
// of the closes' log returns (NaN with fewer than two); the share of history at or below x (NaN
// when empty); the IV at |delta| = target, linear between the options either side of it (NaN when
// none is).
double realized_vol(std::span<const double> closes, double periods_per_year = 252.0);
double percentile_rank(std::span<const double> history, double x);
double iv_at_delta(std::span<const double> deltas, std::span<const double> ivs, double target);

/** Raw SVI (Gatheral): the total implied variance (IV squared times years) at log-moneyness
 * k = ln(strike / forward) is a + b (rho (k - m) + sqrt((k - m)^2 + sigma^2)). */
struct Svi {
    double a = 0.0;
    double b = 0.0;
    double rho = 0.0;
    double m = 0.0;
    double sigma = 0.0;

    [[nodiscard]] double total_variance(double k) const {
        const double x = k - m;
        return a + (b * ((rho * x) + std::sqrt((x * x) + (sigma * sigma))));
    }
    /** No butterfly arbitrage from log-moneyness lo to hi: the total variance positive and
     * Gatheral's g (the density over the normal one) not negative, at 101 points. */
    [[nodiscard]] bool butterfly_free(double lo, double hi) const;
    /** No calendar arbitrage against a smile of a later expiry: its total variance at least this
     * one's from log-moneyness lo to hi, at 101 points. */
    [[nodiscard]] bool calendar_free(const Svi& later, double lo, double hi) const;
};

struct SviFit {
    Svi svi;
    double error = 0.0; // the root mean square of the residuals, each over its half spread
    int points = 0;     // fitted to; 0: none
    bool butterfly_free = false; // from the least log-moneyness fitted to the greatest
};

/** Raw SVI fitted to total variances w at log-moneyness k, each residual over its half spread (of
 * w), squared and summed: for each (m, sigma) the rest by constrained linear least squares
 * (Zeliade's quasi-explicit fit: total variance never negative, b (1 + |rho|) at most 4), (m,
 * sigma) from a grid then Nelder-Mead; when that has butterfly arbitrage within the points,
 * fitted again with it penalised, kept if then free. The same points, the same fit. None with
 * fewer than 5 points. */
SviFit svi_fit(std::span<const double> k, std::span<const double> w,
               std::span<const double> half_spread);

} // namespace quant
