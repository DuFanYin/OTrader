#pragma once

/** A market's calendar, as data: the days it trades, its hours in its own time zone, when a day's
 * contracts roll in and when its options expire. Each portfolio has one (PortfolioSpec::calendar);
 * what asks "which day is it", "is it open", "when does it close" asks it. */

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string_view>

namespace model {

/** Each half hour's share of a 09:30 to 16:00 session's variance, from the open: the median of
 * each day's shares of the squared one-minute returns, SPY and SPXW 2016 to 2026 (alike). */
inline constexpr std::array<double, 13> kSessionVarianceShares{
    0.160, 0.131, 0.099, 0.080, 0.066, 0.055, 0.047, 0.049, 0.044, 0.055, 0.053, 0.062, 0.100};

struct SessionCalendar {
    std::string_view name;
    std::string_view zone; // IANA, as America/New_York
    bool weekends_closed = true;
    std::span<const std::chrono::year_month_day> holidays; // of first_listed to last_listed
    std::span<const std::chrono::year_month_day> early_closes;
    std::chrono::year first_listed{0};
    std::chrono::year last_listed{0};
    std::chrono::minutes open{0}; // local time of day
    std::chrono::minutes close{24 * 60};
    std::chrono::minutes early_close{24 * 60};
    std::optional<std::chrono::minutes> roll; // when a day's contracts come in; none: never

    /** A day it trades: open on weekends or not one, and not a holiday (outside the years listed
     * every such day counts). */
    [[nodiscard]] bool is_trading_day(std::chrono::year_month_day date) const;
    [[nodiscard]] std::chrono::year_month_day
    next_trading_day(std::chrono::year_month_day date) const;
    /** When it closes on `date`, local time of day: close, or early_close on an early close. */
    [[nodiscard]] std::chrono::minutes session_close(std::chrono::year_month_day date) const;
    /** The instant `time_of_day` local time on `date`, daylight saving included. */
    [[nodiscard]] std::chrono::system_clock::time_point at(std::chrono::year_month_day date,
                                                           std::chrono::minutes time_of_day) const;
    /** When an option expiring on `date` expires: that day's close. */
    [[nodiscard]] std::chrono::system_clock::time_point
    expiry_time(std::chrono::year_month_day date) const;
    /** The local date at t: the trading day it falls on. */
    [[nodiscard]] std::chrono::year_month_day
    date_of(std::chrono::system_clock::time_point t) const;
    /** Minutes from the open to t, on t's local date (negative before the open). */
    [[nodiscard]] int minutes_since_open(std::chrono::system_clock::time_point t) const;
    /** Years from t to `expiry` on a variance clock, not the calendar's: each trading day a 252nd
     * of a year, `overnight` of it from the close before to the open and the rest across the
     * session as kSessionVarianceShares spread it (an early close: the shares of the half hours
     * it trades); days it does not trade add nothing. 0 once expiry has passed. */
    [[nodiscard]] double variance_years(std::chrono::system_clock::time_point t,
                                        std::chrono::system_clock::time_point expiry,
                                        double overnight) const;
};

/** The New York Stock Exchange's (and its options'): 09:30 to 16:00, 13:00 on an early close,
 * holidays listed for 2016 to 2027, contracts rolled in at 08:45. */
extern const SessionCalendar kNyse;

} // namespace model
