#pragma once

/**
 * C++23 equivalent of utilities/utility.py
 * Math and time utilities.
 */

#include <chrono>
#include <cmath>
#include <cstdint>

namespace model {

// Calendar constant (from utility.py)
inline constexpr int ANNUAL_DAYS = 240;

/** Round value to target tick (e.g. round_to(1.234, 0.01) -> 1.23). */
double round_to(double value, double target);

/** A quote's mark: its mid when it is quoted both ways, else the side quoted, else `otherwise`
 * (a price marked before, a last trade). */
inline double mark(double bid, double ask, double otherwise = 0.0) {
    if (bid > 0.0 && ask > 0.0) {
        return 0.5 * (bid + ask);
    }
    if (bid > 0.0) {
        return bid;
    }
    return ask > 0.0 ? ask : otherwise;
}

/** Floor value to target. */
double floor_to(double value, double target);

/** The instant an option expiring on `date` expires: the 16:00 New York close it settles on,
 * daylight saving included. Backtest (OCC symbols) and live (database) both use it. */
std::chrono::system_clock::time_point option_expiry_time(std::chrono::year_month_day date);

/** Years from now to expiry (a year of 365.25 days); 0 once it has passed, else at least 1e-6. */
double years_to_expiry(std::chrono::system_clock::time_point now,
                       std::chrono::system_clock::time_point expiry);

/** The New York calendar date at t: the trading day it falls on. */
std::chrono::year_month_day new_york_date(std::chrono::system_clock::time_point t);

/** Minutes from the 09:30 New York open to t, on t's New York date (negative before the open). */
int minutes_since_open(std::chrono::system_clock::time_point t);

/**
 * Turns timer ticks into one call per bar. The data has a row a minute, so a bar is a minute of
 * event time (the time of the portfolio's last snapshot). Backtest ticks once per bar and live once
 * a second: what runs only when next() is true runs once per bar in both.
 */
class BarGate {
  public:
    /** True on the first tick whose event time is in a later minute than the last true one; false
     * before any snapshot (event time still the epoch). */
    [[nodiscard]] bool next(std::chrono::system_clock::time_point event_time);

  private:
    std::chrono::system_clock::time_point last_bar_{};
};

} // namespace model
