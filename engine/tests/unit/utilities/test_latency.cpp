/** LatencyHistogram: percentiles from quarter-of-a-power-of-two buckets, never past the max. */
#include "utilities/latency.hpp"
#include <cstdint>
#include <gtest/gtest.h>

namespace {

TEST(LatencyHistogram, EmptyReadsAllZero) {
    const utilities::LatencyHistogram h;
    const auto s = h.summary();
    EXPECT_EQ(s.count, 0U);
    EXPECT_EQ(s.p50, 0U);
    EXPECT_EQ(s.p99, 0U);
    EXPECT_EQ(s.max, 0U);
}

TEST(LatencyHistogram, TheSmallestDurationsAreExact) {
    utilities::LatencyHistogram h;
    for (std::uint64_t ns : {0U, 1U, 2U, 3U, 3U, 3U}) {
        h.record(ns);
    }
    const auto s = h.summary();
    EXPECT_EQ(s.count, 6U);
    EXPECT_EQ(s.p50, 2U); // rank 3 of 6: below 4 ns each value is a bucket of its own
    EXPECT_EQ(s.p99, 3U);
    EXPECT_EQ(s.max, 3U);
}

TEST(LatencyHistogram, APercentileIsWithinItsBucketAndNeverPastTheLargest) {
    utilities::LatencyHistogram h;
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        h.record(i * 1000); // 1 us .. 1 ms
    }
    const auto s = h.summary();
    EXPECT_EQ(s.count, 1000U);
    EXPECT_EQ(s.max, 1'000'000U);
    EXPECT_GE(s.p50, 500'000U);
    EXPECT_LE(s.p50, 500'000U * 120 / 100); // a quarter-power bucket: within about 19%
    EXPECT_GE(s.p99, 990'000U);
    EXPECT_LE(s.p99, s.max); // the top bucket's edge is capped at the largest seen
    EXPECT_EQ(s.p999, s.max);
}

TEST(LatencyHistogram, OneHugeSampleLandsInTheLastBucket) {
    utilities::LatencyHistogram h;
    h.record(std::uint64_t{1} << 50); // past 2^40 ns: the last bucket holds it
    const auto s = h.summary();
    EXPECT_EQ(s.count, 1U);
    EXPECT_EQ(s.max, std::uint64_t{1} << 50);
    EXPECT_EQ(s.p50, (std::uint64_t{1} << 41) - 1); // that bucket's top: the range ends there
}

} // namespace
