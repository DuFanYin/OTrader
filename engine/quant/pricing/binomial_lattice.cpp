#include "../kernel.h"
#include <cmath>
#include <vector>

namespace quant {

double binomial_price(double spot, double strike, double rate, double vol, double maturity, int steps, bool is_call) {
    if (spot <= 0.0 || strike <= 0.0 || maturity <= 0.0 || vol <= 0.0 || steps <= 0) {
        return 0.0;
    }
    const double dt = maturity / static_cast<double>(steps);
    const double u = std::exp(vol * std::sqrt(dt));
    const double d = 1.0 / u;
    const double disc = std::exp(-rate * dt);
    const double p = (std::exp(rate * dt) - d) / (u - d);

    std::vector<double> values(steps + 1, 0.0);
    for (int i = 0; i <= steps; ++i) {
        const double st = spot * std::pow(u, i) * std::pow(d, steps - i);
        values[i] = is_call ? (st > strike ? st - strike : 0.0)
                             : (strike > st ? strike - st : 0.0);
    }
    for (int t = steps - 1; t >= 0; --t) {
        for (int i = 0; i <= t; ++i) {
            values[i] = disc * (p * values[i + 1] + (1.0 - p) * values[i]);
        }
    }
    return values[0];
}

double binomial_price(double spot, double strike, double rate, double vol, double maturity, int steps) {
    return binomial_price(spot, strike, rate, vol, maturity, steps, true);
}

double binomial_american_price(double spot, double strike, double rate, double vol, double maturity, int steps, double dividend_yield) {
    if (spot <= 0.0 || strike <= 0.0 || maturity <= 0.0 || vol <= 0.0 || steps <= 0) {
        return clamp_nonnegative(spot - strike);
    }
    const double r_eff = rate - dividend_yield;
    const double dt   = maturity / static_cast<double>(steps);
    const double u    = std::exp(vol * std::sqrt(dt));
    const double d    = 1.0 / u;
    const double disc = std::exp(-rate * dt);
    const double p    = (std::exp(r_eff * dt) - d) / (u - d);

    std::vector<double> values(steps + 1, 0.0);
    for (int i = 0; i <= steps; ++i) {
        const double st = spot * std::pow(u, i) * std::pow(d, steps - i);
        values[i] = clamp_nonnegative(st - strike);
    }
    for (int t = steps - 1; t >= 0; --t) {
        for (int i = 0; i <= t; ++i) {
            const double st         = spot * std::pow(u, i) * std::pow(d, t - i);
            const double hold       = disc * (p * values[i + 1] + (1.0 - p) * values[i]);
            const double intrinsic  = clamp_nonnegative(st - strike);
            values[i] = std::max(hold, intrinsic);
        }
    }
    return values[0];
}

}
