#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <chrono>
#include <gtest/gtest.h>

namespace {

using namespace std::chrono;

auto utc(year_month_day date, hours hour) -> system_clock::time_point {
    return sys_days{date} + hour;
}

// An OCC code names the option the backtest data has; it expires at the 16:00 New York close:
// 21:00 UTC in winter, 20:00 UTC in summer.
TEST(OccExpiry, ExpiresAtTheNewYorkCloseInWinterAndSummer) {
    const auto winter = model::parse_occ_code("SPY", "250307C00600000");
    ASSERT_TRUE(winter.has_value());
    EXPECT_EQ(model::option_expiry_time(winter->expiry), utc(2025y / March / 7d, 21h));
    EXPECT_DOUBLE_EQ(winter->strike, 600.0);
    EXPECT_EQ(model::option_symbol(*winter), "SPY-20250307-C-600");

    const auto summer = model::parse_occ_code("SPXW", "250804P05000000");
    ASSERT_TRUE(summer.has_value());
    EXPECT_EQ(model::option_expiry_time(summer->expiry), utc(2025y / August / 4d, 20h));
    EXPECT_EQ(summer->type, model::OptionType::PUT);
    EXPECT_EQ(model::occ_code(*summer), "250804P05000000"); // and back
}

TEST(OccExpiry, AnImpossibleDateIsRejected) {
    EXPECT_FALSE(model::parse_occ_code("SPY", "251332C00600000").has_value());
}

} // namespace
