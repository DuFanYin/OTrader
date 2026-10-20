// Recombining lattices with dividend yield, European or American exercise.
// Trinomial: Boyle moment-matching tree, ported from QF-205
// (src/option_calculator/methods/trinomial.py); the rolling single-array
// backward induction follows Option-Pricing (trees/trinomialTree.cpp).

#include "../kernel.h"

#include <cmath>
#include <limits>
#include <vector>

namespace quant {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

}  // namespace

double trinomial_price(double spot, double strike, double rate, double dividend_yield, double vol,
                       double maturity, int steps, bool is_call, bool american) {
    if (spot <= 0.0 || strike <= 0.0 || vol <= 0.0 || steps < 1) return kNaN;
    if (maturity <= 0.0) return payoff(spot, strike, is_call);

    const double dt   = maturity / steps;
    const double disc = std::exp(-rate * dt);
    const double nu   = rate - dividend_yield - 0.5 * vol * vol;
    const double dx   = vol * std::sqrt(3.0 * dt);
    const double pu   = 1.0 / 6.0 + nu * std::sqrt(dt) / (2.0 * vol * std::sqrt(3.0));
    const double pm   = 2.0 / 3.0;
    const double pd   = 1.0 - pu - pm;
    if (pu < -1e-12 || pd < -1e-12) return kNaN;   // drift too large for this dt: needs more steps

    // Node j at step i sits at S = spot * exp(dx * (j - i)), j = 0..2i.
    std::vector<double> v(2 * steps + 1);
    for (int j = 0; j <= 2 * steps; ++j) v[j] = payoff(spot * std::exp(dx * (j - steps)), strike, is_call);

    for (int i = steps - 1; i >= 0; --i) {
        // In place: v[j] (new) only reads v[j..j+2] (old), so ascending j is safe.
        for (int j = 0; j <= 2 * i; ++j) {
            double cont = disc * (pd * v[j] + pm * v[j + 1] + pu * v[j + 2]);
            if (american) cont = std::max(cont, payoff(spot * std::exp(dx * (j - i)), strike, is_call));
            v[j] = cont;
        }
    }
    return v[0];
}

double binomial_crr_price(double spot, double strike, double rate, double dividend_yield, double vol,
                          double maturity, int steps, bool is_call, bool american) {
    if (spot <= 0.0 || strike <= 0.0 || vol <= 0.0 || steps < 1) return kNaN;
    if (maturity <= 0.0) return payoff(spot, strike, is_call);

    const double dt   = maturity / steps;
    const double u    = std::exp(vol * std::sqrt(dt));
    const double d    = 1.0 / u;
    const double disc = std::exp(-rate * dt);
    const double p    = (std::exp((rate - dividend_yield) * dt) - d) / (u - d);
    if (p < 0.0 || p > 1.0) return kNaN;

    const double log_u = std::log(u);
    std::vector<double> v(steps + 1);
    for (int j = 0; j <= steps; ++j) v[j] = payoff(spot * std::exp(log_u * (2 * j - steps)), strike, is_call);

    for (int i = steps - 1; i >= 0; --i) {
        for (int j = 0; j <= i; ++j) {
            double cont = disc * (p * v[j + 1] + (1.0 - p) * v[j]);
            if (american) cont = std::max(cont, payoff(spot * std::exp(log_u * (2 * j - i)), strike, is_call));
            v[j] = cont;
        }
    }
    return v[0];
}

}  // namespace quant
