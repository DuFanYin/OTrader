#pragma once

/** Rounding, quotes' marks, the exchange's calendar and clock, and dates in text. */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace model {

// Calendar constant
inline constexpr int ANNUAL_DAYS = 240;

/** Round value to target tick (e.g. round_to(1.234, 0.01) -> 1.23). */
double round_to(double value, double target);

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


/** A day the New York Stock Exchange trades: a weekday that is not one of its holidays. The
 * holidays are listed for 2020 to 2027; outside those years every weekday counts. */
bool is_trading_day(std::chrono::year_month_day date);

/** The trading day after `date`. */
std::chrono::year_month_day next_trading_day(std::chrono::year_month_day date);

/** When the exchange closes on `date`, New York time of day: 16:00, or 13:00 on an early close
 * (the day before Independence Day, the day after Thanksgiving, Christmas Eve). */
std::chrono::minutes session_close(std::chrono::year_month_day date);

/** The instant `time_of_day` New York time on `date`, daylight saving included. */
std::chrono::system_clock::time_point new_york_time(std::chrono::year_month_day date,
                                                    std::chrono::minutes time_of_day);

/** The instant an option expiring on `date` expires: the New York close it settles on
 * (session_close), daylight saving included. Backtest (OCC symbols) and live (database) both use
 * it. */
std::chrono::system_clock::time_point option_expiry_time(std::chrono::year_month_day date);

/** Years from now to expiry (a year of 365.25 days); 0 once it has passed, else at least 1e-6. */
double years_to_expiry(std::chrono::system_clock::time_point now,
                       std::chrono::system_clock::time_point expiry);

/** The New York calendar date at t: the trading day it falls on. */
std::chrono::year_month_day new_york_date(std::chrono::system_clock::time_point t);

/** Minutes from the 09:30 New York open to t, on t's New York date (negative before the open). */
int minutes_since_open(std::chrono::system_clock::time_point t);

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
