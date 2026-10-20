/** The session calendar's variance clock: a trading day is a 252nd of a year, split between the
 * night before and the session's half hours as their shares say. */

#include "model/session_calendar.hpp"

#include <chrono>
#include <gtest/gtest.h>

namespace {

using namespace std::chrono;
using model::kNyse;

constexpr double kDay = 1.0 / 252.0;

auto at(year_month_day date, int hour, int minute) -> system_clock::time_point {
    return kNyse.at(date, hours{hour} + minutes{minute});
}

TEST(VarianceClock, ASessionFromItsOpenIsTheDayLessItsNight) {
    const year_month_day mon = 2025y / March / 3d;
    EXPECT_NEAR(kNyse.variance_years(at(mon, 9, 30), kNyse.expiry_time(mon), 0.4), 0.6 * kDay,
                1e-12);
    // Before the open, the night is still to come.
    EXPECT_NEAR(kNyse.variance_years(at(mon, 8, 0), kNyse.expiry_time(mon), 0.4), kDay, 1e-12);
}

TEST(VarianceClock, TheMorningHoldsMoreThanItsShareOfTheHours) {
    const year_month_day mon = 2025y / March / 3d;
    const double left_at_noon = kNyse.variance_years(at(mon, 12, 0), kNyse.expiry_time(mon), 0.0);
    double first_five = 0.0;
    for (int b = 0; b < 5; ++b) {
        first_five += model::kSessionVarianceShares[b];
    }
    double all = 0.0;
    for (const double s : model::kSessionVarianceShares) {
        all += s;
    }
    EXPECT_NEAR(left_at_noon, (1.0 - (first_five / all)) * kDay, 1e-12);
    EXPECT_LT(left_at_noon, (4.0 / 6.5) * kDay); // less than the hours left would say
}

TEST(VarianceClock, AWeekendAddsNothingButTheNightBeforeMonday) {
    const year_month_day fri = 2025y / March / 7d;
    const year_month_day mon = 2025y / March / 10d;
    const double from_close = kNyse.variance_years(kNyse.expiry_time(fri), kNyse.expiry_time(mon),
                                                   0.4);
    EXPECT_NEAR(from_close, kDay, 1e-12);
    EXPECT_EQ(kNyse.variance_years(kNyse.expiry_time(mon), at(mon, 10, 0), 0.4), 0.0);
}

} // namespace
