/** parse_date: the date a text starts with, YYYY-MM-DD or YYYYMMDD. */

#include "model/utility.hpp"
#include <chrono>
#include <gtest/gtest.h>

namespace {

using namespace std::chrono;

TEST(ParseDate, ReadsIsoDatesAndTimestamps) {
    EXPECT_EQ(model::parse_date("2026-10-14"), 2026y / October / 14d);
    EXPECT_EQ(model::parse_date("2026-10-14T13:30:00Z"), 2026y / October / 14d);
    EXPECT_EQ(model::parse_date("2026-10-14 13:30:00"), 2026y / October / 14d);
}

TEST(ParseDate, ReadsIbDates) {
    EXPECT_EQ(model::parse_date("20261014"), 2026y / October / 14d);
    EXPECT_EQ(model::parse_date("20261014 16:00:00 US/Eastern"), 2026y / October / 14d);
}

TEST(ParseDate, RefusesDatesThatDoNotExist) {
    EXPECT_FALSE(model::parse_date("2026-02-30"));
    EXPECT_FALSE(model::parse_date("2026-13-01"));
    EXPECT_FALSE(model::parse_date("20260230"));
    EXPECT_EQ(model::parse_date("2028-02-29"), 2028y / February / 29d);
}

TEST(ParseDate, RefusesGarbage) {
    EXPECT_FALSE(model::parse_date(""));
    EXPECT_FALSE(model::parse_date("expiry"));
    EXPECT_FALSE(model::parse_date("2026-1-5"));
    EXPECT_FALSE(model::parse_date(" 2026-10-14"));
    EXPECT_FALSE(model::parse_date("2026/10/14"));
    EXPECT_FALSE(model::parse_date("202610"));
    EXPECT_FALSE(model::parse_date("2026-10-1x"));
}

} // namespace
