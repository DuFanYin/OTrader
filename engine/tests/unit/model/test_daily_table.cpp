/**
 * Values by day, read from data/daily's files: a series a file, by date whatever the order written;
 * what came before a day, and the next date on or after one. A line that is not a date and a number,
 * or a date given twice, is refused; no folder is no values.
 */
#include "model/daily_table.hpp"
#include "model/object.hpp"
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono;

class DailyTableFiles : public ::testing::Test {
  protected:
    std::filesystem::path dir_ = std::filesystem::temp_directory_path() /
                                 ("optra_daily_" + std::to_string(::getpid()));
    void SetUp() override { std::filesystem::create_directories(dir_); }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    void write(const std::string& name, const std::string& text) {
        std::ofstream(dir_ / name) << text;
    }
};

TEST_F(DailyTableFiles, ASeriesAFileByDate) {
    write("VIX.csv", "# Cboe, the close\ndate,value\n2025-03-05,21.5\n2025-03-03,19\n2025-03-04,20\n");
    write("notes.txt", "not a series");
    const model::DailyTable t = model::DailyTable::load(dir_);
    EXPECT_EQ(t.before("VIX", 2025y / March / 5, 10), (std::vector<double>{19, 20})); // not the day
    EXPECT_EQ(t.before("VIX", 2025y / March / 6, 2), (std::vector<double>{20, 21.5}));
    EXPECT_TRUE(t.before("VIX", 2025y / March / 3, 5).empty());
    EXPECT_TRUE(t.before("VIX9D", 2025y / March / 6, 5).empty());
    EXPECT_EQ(t.next("VIX", 2025y / March / 4), 2025y / March / 4);
    EXPECT_EQ(t.next("VIX", 2025y / March / 6), std::nullopt);
    EXPECT_EQ(t.last_before("VIX", 2025y / March / 6), 2025y / March / 5);
    EXPECT_EQ(t.last_before("VIX", 2025y / March / 3), std::nullopt);
}

TEST_F(DailyTableFiles, ABadLineOrADateTwiceIsRefused) {
    write("A.csv", "2025-03-03,x\n");
    EXPECT_THROW((void)model::DailyTable::load(dir_), std::runtime_error);
    std::filesystem::remove(dir_ / "A.csv");
    write("B.csv", "2025-03-03,1\n2025-03-03,2\n");
    EXPECT_THROW((void)model::DailyTable::load(dir_), std::runtime_error);
    EXPECT_TRUE(model::DailyTable::load(dir_ / "none").empty());
}

TEST(TreasuryCurve, TheYieldsOfTheDayBeforeContinuouslyCompounded) {
    model::DailyTable daily;
    const year_month_day mon = 2025y / March / 3d;
    const year_month_day tue = 2025y / March / 4d;
    for (const auto* series : {"DGS1MO", "DGS3MO", "DGS6MO", "DGS1"}) {
        daily.set(series, {{mon, 4.0}, {tue, 5.0}});
    }
    EXPECT_FALSE(model::RateCurve::treasury(daily, tue)); // no DGS2
    daily.set("DGS2", {{mon, 4.0}, {tue, 5.0}});
    const auto curve = model::RateCurve::treasury(daily, tue);
    ASSERT_TRUE(curve);
    EXPECT_EQ(curve->years, (std::vector<double>{1.0 / 12.0, 0.25, 0.5, 1.0, 2.0}));
    EXPECT_NEAR(curve->at(0.5), 2.0 * std::log(1.02), 1e-15); // Monday's 4%, compounded twice a year
}

} // namespace
