#pragma once

/** Black–Scholes IV and Greeks (LetsBeRational). */

#include <chrono>
#include <optional>
#include <string>

namespace utilities {

/** Per-unit Greeks; theta = per day, vega = per 1% vol move (aligned with py_vollib). */
struct BsGreeks {
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double vega = 0.0;
};

/** Pick IV input price (bid/ask/mid). */
double pick_iv_input_price(double bid, double ask, const std::string& mode);

/** Years to expiry. */
double years_to_expiry(std::chrono::system_clock::time_point now,
                       const std::optional<std::chrono::system_clock::time_point>& expiry);

/** BS Greeks from vol. */
BsGreeks bs_greeks(bool is_call, double spot, double strike, double time_to_expiry_years,
                   double risk_free_rate, double sigma);

/** IV from price (LetsBeRational). */
double implied_volatility_from_price(double option_price, double spot, double strike,
                                     double time_to_expiry_years, bool is_call);

} // namespace utilities
