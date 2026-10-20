#pragma once

/** Rounding, quotes' marks, time to expiry, and dates in text (the calendar: session_calendar.hpp). */

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace model {

/** Rounded to `digits` decimals, halves away from zero (round_digits(1.235, 2) -> 1.24); scaled by
 * an exact power of ten, not divided by a tick such as 0.01 that no double holds. */
inline double round_digits(double value, int digits) {
    if (digits < 0) {
        return value;
    }
    // Exact powers of ten, the values std::pow gives for them, without its call.
    static constexpr std::array<double, 9> kPowersOfTen{1e0, 1e1, 1e2, 1e3, 1e4,
                                                        1e5, 1e6, 1e7, 1e8};
    const double factor = std::cmp_less(digits, kPowersOfTen.size())
                              ? kPowersOfTen[static_cast<std::size_t>(digits)]
                              : std::pow(10.0, digits);
    const double scaled = value * factor;
    const double rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
    return rounded / factor;
}

/** A quote's mark: its mid when it is quoted both ways, else the side quoted, else `otherwise`
 * (a price marked before, a last trade). */
inline double mark(double bid, double ask, double otherwise = 0.0) {
    if (bid > 0.0 && ask > 0.0) { // crossed (bid above ask): no valid quote, as for a fill
        return bid <= ask ? 0.5 * (bid + ask) : otherwise;
    }
    if (bid > 0.0) {
        return bid;
    }
    return ask > 0.0 ? ask : otherwise;
}


/** A year of calendar time, as Cboe counts it for the VIX: 365 days. */
inline constexpr double kSecondsAYear = 365.0 * 24.0 * 3600.0;
/** Years from now to expiry (kSecondsAYear); 0 once it has passed, else at least 1e-6. */
double years_to_expiry(std::chrono::system_clock::time_point now,
                       std::chrono::system_clock::time_point expiry);

/** The text a time is kept in (UTC, to the second), as the UI shows it. */
std::string datetime_to_str(std::chrono::system_clock::time_point tp);

/** The date a text starts with, YYYY-MM-DD (an ISO date or timestamp) or YYYYMMDD (IB's); what
 * follows it is not read. nullopt when it does not start with one or the date does not exist. */
std::optional<std::chrono::year_month_day> parse_date(std::string_view text);

/**
 * Turns timer ticks into one call every `every` bars. The data has a row a minute, so a bar is a
 * minute of event time (the time of the portfolio's last snapshot). Backtest ticks once per bar and
 * live once a second: what runs only when next() is true runs as often in both.
 */
class BarGate {
  public:
    explicit BarGate(int every = 1) : every_(every) {}
    /** True on the first tick in a later minute than the last one counted, every `every`-th;
     * false before any snapshot (event time still the epoch). */
    [[nodiscard]] bool next(std::chrono::system_clock::time_point event_time);

  private:
    std::chrono::system_clock::time_point last_bar_{};
    int every_ = 1;
    int bars_ = 0; // counted since it last opened
};

} // namespace model
