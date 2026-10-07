/**
 * ArrowParquetLoader contract: accepts the column layouts real writers produce, rejects types the
 * frame readers would mis-cast.
 * Link: backtest_engines (Arrow + Parquet). Writes temporary Parquet files.
 */
#include "utilities/parquet_loader.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int64_t kMinuteUs = 60'000'000;
constexpr int64_t kT0Us = 1'754'314'260'000'000; // 2025-08-04 13:31:00 UTC

struct Rows {
    std::vector<std::string> symbols;
    std::vector<int64_t> ts_us;
};

// 3 timesteps x 2 options.
auto sample_rows() -> Rows {
    Rows r;
    for (int t = 0; t < 3; ++t) {
        for (const char* s : {"250804C05000000", "250804P05000000"}) {
            r.symbols.emplace_back(s);
            r.ts_us.push_back(kT0Us + (t * kMinuteUs));
        }
    }
    return r;
}

template <typename Builder, typename T>
auto build(std::vector<T> const& values) -> std::shared_ptr<arrow::Array> {
    Builder b;
    EXPECT_TRUE(b.AppendValues(values).ok());
    return b.Finish().ValueOrDie();
}

// Writes the backtest schema; `large_symbols` mimics polars, `ts_as_int` a broken writer.
auto write_file(fs::path const& path, bool large_symbols, bool ts_as_int, int64_t row_group_rows)
    -> void {
    const Rows r = sample_rows();
    const auto n = static_cast<int64_t>(r.symbols.size());
    std::shared_ptr<arrow::Array> sym;
    if (large_symbols) {
        arrow::LargeStringBuilder b;
        for (auto const& s : r.symbols) {
            ASSERT_TRUE(b.Append(s).ok());
        }
        sym = b.Finish().ValueOrDie();
    } else {
        arrow::StringBuilder b;
        for (auto const& s : r.symbols) {
            ASSERT_TRUE(b.Append(s).ok());
        }
        sym = b.Finish().ValueOrDie();
    }
    std::shared_ptr<arrow::Array> ts;
    std::shared_ptr<arrow::DataType> ts_type;
    if (ts_as_int) {
        ts = build<arrow::Int64Builder>(r.ts_us);
        ts_type = arrow::int64();
    } else {
        ts_type = arrow::timestamp(arrow::TimeUnit::MICRO, "America/New_York");
        arrow::TimestampBuilder b(ts_type, arrow::default_memory_pool());
        ASSERT_TRUE(b.AppendValues(r.ts_us).ok());
        ts = b.Finish().ValueOrDie();
    }
    const std::vector<double> px(static_cast<size_t>(n), 1.5);
    const std::vector<int64_t> sz(static_cast<size_t>(n), 10);
    auto dbl = build<arrow::DoubleBuilder>(px);
    auto i64 = build<arrow::Int64Builder>(sz);
    auto schema = arrow::schema({
        arrow::field("symbol", sym->type()),
        arrow::field("ts_recv", ts_type),
        arrow::field("bid_px", arrow::float64()),
        arrow::field("ask_px", arrow::float64()),
        arrow::field("bid_sz", arrow::int64()),
        arrow::field("ask_sz", arrow::int64()),
        arrow::field("underlying_bid_px", arrow::float64()),
        arrow::field("underlying_ask_px", arrow::float64()),
        arrow::field("underlying_bid_sz", arrow::int64()),
        arrow::field("underlying_ask_sz", arrow::int64()),
    });
    auto table = arrow::Table::Make(schema, {sym, ts, dbl, dbl, i64, i64, dbl, dbl, i64, i64});
    auto out = arrow::io::FileOutputStream::Open(path.string()).ValueOrDie();
    auto props = parquet::ArrowWriterProperties::Builder().store_schema()->build();
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out,
                                           row_group_rows, parquet::default_writer_properties(),
                                           props)
                    .ok());
    ASSERT_TRUE(out->Close().ok());
}

class ParquetLoaderTest : public ::testing::Test {
  protected:
    fs::path dir_ =
        fs::temp_directory_path() /
        ("otrader_loader_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    void SetUp() override { fs::create_directories(dir_); }
    void TearDown() override { fs::remove_all(dir_); }
};

auto frames(backtest::ArrowParquetLoader const& loader) -> std::vector<int64_t> {
    std::vector<int64_t> rows_per_step;
    loader.iter_timesteps([&](backtest::TimestepFrameColumnar const& f) {
        for (int64_t r = 0; r < f.num_rows; ++r) {
            EXPECT_FALSE(backtest::detail::StringAt(f.arr_sym, f.row_index(r)).empty());
        }
        rows_per_step.push_back(f.num_rows);
        return true;
    });
    return rows_per_step;
}

TEST_F(ParquetLoaderTest, LargeStringSymbolsAndManyRowGroupsLoadEveryRow) {
    // polars writes symbol as large_string; 1-row row groups give the reader many chunks.
    const fs::path p = dir_ / "20250804.parquet";
    write_file(p, /*large_symbols=*/true, /*ts_as_int=*/false, /*row_group_rows=*/1);
    backtest::ArrowParquetLoader loader;
    ASSERT_TRUE(loader.load(p.string())) << loader.last_error();
    std::unordered_set<std::string> symbols;
    loader.collect_symbols(symbols);
    EXPECT_EQ(symbols, (std::unordered_set<std::string>{"250804C05000000", "250804P05000000"}));
    EXPECT_EQ(frames(loader), (std::vector<int64_t>{2, 2, 2}));
}

TEST_F(ParquetLoaderTest, PlainStringSymbolsLoad) {
    const fs::path p = dir_ / "20250804.parquet";
    write_file(p, /*large_symbols=*/false, /*ts_as_int=*/false, /*row_group_rows=*/1'048'576);
    backtest::ArrowParquetLoader loader;
    ASSERT_TRUE(loader.load(p.string())) << loader.last_error();
    EXPECT_EQ(frames(loader), (std::vector<int64_t>{2, 2, 2}));
}

TEST_F(ParquetLoaderTest, WrongColumnTypeIsRejectedWithReason) {
    const fs::path p = dir_ / "20250804.parquet";
    write_file(p, /*large_symbols=*/false, /*ts_as_int=*/true, /*row_group_rows=*/1'048'576);
    backtest::ArrowParquetLoader loader;
    EXPECT_FALSE(loader.load(p.string()));
    EXPECT_NE(loader.last_error().find("ts_recv"), std::string::npos) << loader.last_error();
}

TEST_F(ParquetLoaderTest, MissingFileIsRejectedWithReason) {
    backtest::ArrowParquetLoader loader;
    EXPECT_FALSE(loader.load((dir_ / "nope.parquet").string()));
    EXPECT_FALSE(loader.last_error().empty());
}

} // namespace
