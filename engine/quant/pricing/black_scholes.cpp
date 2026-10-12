#include "../kernel.h"
#include <cmath>

namespace quant {

double bs_closed_form_price(double spot, double strike, double rate, double vol, double maturity, bool is_call) {
    if (spot <= 0.0 || strike <= 0.0 || maturity <= 0.0 || vol <= 0.0) {
        return is_call ? (spot > strike ? spot - strike : 0.0)
                       : (strike > spot ? strike - spot : 0.0);
    }
    const double sqrt_t = std::sqrt(maturity);
    const double d1 = (std::log(spot / strike) + (rate + 0.5 * vol * vol) * maturity) / (vol * sqrt_t);
    const double d2 = d1 - vol * sqrt_t;
    if (is_call)
        return spot * norm_cdf(d1) - strike * std::exp(-rate * maturity) * norm_cdf(d2);
    else
        return strike * std::exp(-rate * maturity) * norm_cdf(-d2) - spot * norm_cdf(-d1);
}

// Backward-compat overload (call only) — keeps all existing callers working.
double bs_closed_form_price(double spot, double strike, double rate, double vol, double maturity) {
    return bs_closed_form_price(spot, strike, rate, vol, maturity, true);
}

double bs_delta(double spot, double strike, double rate, double vol, double maturity) {
    if (maturity <= 0.0 || vol <= 0.0 || spot <= 0.0 || strike <= 0.0) {
        return spot > strike ? 1.0 : 0.0;
    }
    const double d1 = (std::log(spot / strike) + (rate + 0.5 * vol * vol) * maturity)
        / (vol * std::sqrt(maturity));
    return norm_cdf(d1);
}

double digital_call_bs_price(double spot, double strike, double rate, double vol, double maturity, double dividend_yield) {
    if (spot <= 0.0 || strike <= 0.0 || maturity <= 0.0 || vol <= 0.0) {
        return spot > strike ? std::exp(-rate * maturity) : 0.0;
    }
    const double sqrt_t = std::sqrt(maturity);
    const double d2 = (std::log(spot / strike) + (rate - dividend_yield - 0.5 * vol * vol) * maturity) / (vol * sqrt_t);
    return std::exp(-rate * maturity) * norm_cdf(d2);
}

}
