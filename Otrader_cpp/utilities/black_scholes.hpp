#pragma once

/**
 * Black–Scholes IV and Greeks in utilities (shared by portfolio, backtest, live).
 * IV via LetsBeRational; Greeks per-unit (theta per day, vega per 1% vol).
 */

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

/** Choose option price from bid/ask for IV input. mode: "bid" | "ask" | "mid" (default mid). */
double pick_iv_input_price(double bid, double ask, const std::string& mode);

/** Years to expiry from now to option_expiry; returns >= min_t (1e-6). */
double years_to_expiry(std::chrono::system_clock::time_point now,
                       const std::optional<std::chrono::system_clock::time_point>& expiry);

/** Black–Scholes Greeks given volatility (per-unit). */
BsGreeks bs_greeks(bool is_call, double spot, double strike, double time_to_expiry_years,
                   double risk_free_rate, double sigma);

/**
 * Implied volatility from option price (LetsBeRational).
 * Returns IV or 0 if invalid/non-finite.
 */
double implied_volatility_from_price(double option_price, double spot, double strike,
                                     double time_to_expiry_years, bool is_call);

} // namespace utilities
