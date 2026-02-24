#pragma once

/**
 * C++20 equivalent of utilities/utility.py
 * Math and time utilities.
 */

#include <chrono>
#include <cmath>
#include <cstdint>

namespace utilities {

// Calendar constant (from utility.py)
inline constexpr int ANNUAL_DAYS = 240;

/** Round value to target tick (e.g. round_to(1.234, 0.01) -> 1.23). */
double round_to(double value, double target);

/** Floor value to target. */
double floor_to(double value, double target);

/** Ceil value to target. */
double ceil_to(double value, double target);

/** Get number of decimal digits in value. */
int get_digits(double value);

/**
 * Calculate calendar days to expiry from now to option_expiry.
 * Simplified version (no exchange calendar); for full calendar use external lib.
 */
int calculate_days_to_expiry(std::chrono::system_clock::time_point option_expiry);

/** Overload: optional expiry (returns 0 if null/invalid). */
int calculate_days_to_expiry(std::chrono::system_clock::time_point* option_expiry);

}  // namespace utilities
