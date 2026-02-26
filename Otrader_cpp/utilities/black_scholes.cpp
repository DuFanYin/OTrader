#include "black_scholes.hpp"
#include "lets_be_rational_api.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>

namespace utilities {

namespace {

constexpr double kMinVol = 1e-6;
constexpr double kMaxVol = 5.0;
constexpr double kMinT = 1e-6;

double normal_pdf(double x) {
    static constexpr double inv_sqrt_2pi = 0.3989422804014327;
    return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

double normal_cdf(double x) {
    static const double inv_sqrt_2 = 0.70710678118654752440;
    return 0.5 * (1.0 + std::erf(x * inv_sqrt_2));
}

double bs_vega_raw(double s, double k, double t, double r, double sigma) {
    if (s <= 0.0 || k <= 0.0 || t <= 0.0 || sigma <= 0.0) {
        return 0.0;
    }
    const double sqrt_t = std::sqrt(t);
    const double d1 =
        (std::log(s / k) + (r + 0.5 * sigma * sigma) * t) / (sigma * sqrt_t);
    return s * normal_pdf(d1) * sqrt_t;
}

} // namespace

double pick_iv_input_price(double bid, double ask, const std::string& mode) {
    std::string m = mode;
    std::ranges::transform(m, m.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (m == "bid") {
        return bid > 0.0 ? bid : 0.0;
    }
    if (m == "ask") {
        return ask > 0.0 ? ask : 0.0;
    }
    return (bid > 0.0 && ask > 0.0) ? (bid + ask) / 2.0 : (bid > 0.0 ? bid : ask);
}

double years_to_expiry(std::chrono::system_clock::time_point now,
                       const std::optional<std::chrono::system_clock::time_point>& expiry) {
    if (!expiry.has_value()) {
        return 0.0;
    }
    const auto dt = expiry.value() - now;
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(dt).count();
    if (secs <= 0) {
        return 0.0;
    }
    return std::max(kMinT, static_cast<double>(secs) / (365.25 * 24.0 * 3600.0));
}

BsGreeks bs_greeks(bool is_call, double spot, double strike, double time_to_expiry_years,
                   double risk_free_rate, double sigma) {
    BsGreeks g;
    if (spot <= 0.0 || strike <= 0.0 || time_to_expiry_years <= 0.0 || sigma <= 0.0) {
        return g;
    }
    const double sqrt_t = std::sqrt(time_to_expiry_years);
    const double d1 = (std::log(spot / strike) +
                       (risk_free_rate + 0.5 * sigma * sigma) * time_to_expiry_years) /
                      (sigma * sqrt_t);
    const double d2 = d1 - (sigma * sqrt_t);
    const double pdf = normal_pdf(d1);
    const double df = std::exp(-risk_free_rate * time_to_expiry_years);

    g.delta = is_call ? normal_cdf(d1) : (normal_cdf(d1) - 1.0);
    g.gamma = pdf / (spot * sigma * sqrt_t);
    const double theta_annual =
        is_call
            ? ((-(spot * pdf * sigma) / (2.0 * sqrt_t)) - (risk_free_rate * strike * df * normal_cdf(d2)))
            : ((-(spot * pdf * sigma) / (2.0 * sqrt_t)) +
               (risk_free_rate * strike * df * normal_cdf(-d2)));
    g.theta = theta_annual / 365.0;
    g.vega = bs_vega_raw(spot, strike, time_to_expiry_years, risk_free_rate, sigma) / 100.0;
    return g;
}

double implied_volatility_from_price(double option_price, double spot, double strike,
                                     double time_to_expiry_years, bool is_call) {
    if (option_price <= 0.0 || spot <= 0.0 || strike <= 0.0 || time_to_expiry_years <= 0.0) {
        return 0.0;
    }
    const double q = is_call ? 1.0 : -1.0;
    double iv =
        implied_volatility_from_a_transformed_rational_guess(
            option_price, spot, strike, time_to_expiry_years, q);
    if (!std::isfinite(iv) || iv <= 0.0) {
        return 0.0;
    }
    return std::min(iv, kMaxVol);
}

} // namespace utilities
