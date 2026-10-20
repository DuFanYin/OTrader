/**
 * read_parquet_quotes: takes the column layouts real writers produce (string or large_string
 * symbols, many row groups), refuses a file or a column it cannot read with the reason, and gives
 * the rows in time order with the rows of each time; a null price or size is 0, a null underlying
 * price NaN; a decimal price is its integer times 10^-places. HistoricalData reads a chain the same
 * whatever the file's row order and row groups, and prices a day with the risk-free curve its file
 * carries, else at the rate set. The simulated market answers a day's close from that day's file.
 * Writes temporary Parquet files.
 */
#include "adapters/market_data/data_loader.hpp"
#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "adapters/market_data/historical_data.hpp"
#include "adapters/market_data/simulated_market_data.hpp"
#include "model/portfolios.hpp"
#include <algorithm>
#include <array>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/util/key_value_metadata.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <parquet/arrow/writer.h>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int64_t kMinuteUs = 60'000'000;
constexpr int64_t kT0Us = 1'754'314'260'000'000; // 2025-08-04 13:31:00 UTC
constexpr const char* kCall = "250804C05000000";
constexpr const char* kPut = "250804P05000000";

/** A row: its minute after kT0Us, its option, its bid (none: null) and its underlying's bid. */
struct Row {
    int minute;
    const char* symbol;
    std::optional<double> bid = 1.5;
    std::optional<double> underlying_bid = 5000.0;
};

// 3 minutes x 2 options.
auto sample_rows() -> std::vector<Row> {
    std::vector<Row> rows;
    for (int minute = 0; minute < 3; ++minute) {
        rows.push_back({.minute = minute, .symbol = kCall});
        rows.push_back({.minute = minute, .symbol = kPut});
    }
    return rows;
}

template <typename Builder> auto finish(Builder& b) -> std::shared_ptr<arrow::Array> {
    return b.Finish().ValueOrDie();
}

auto append(arrow::DoubleBuilder& b, std::optional<double> value) -> void {
    ASSERT_TRUE((value ? b.Append(*value) : b.AppendNull()).ok());
}

/** The price in cents, as a decimal with two places. */
auto append(arrow::Decimal128Builder& b, std::optional<double> value) -> void {
    ASSERT_TRUE((value ? b.Append(arrow::Decimal128(std::llround(*value * 100))) : b.AppendNull()).ok());
}

struct Layout {
    bool large_symbols = false; // as polars writes them
    bool ts_as_int = false;     // a broken writer
    bool decimal_prices = false; // the option's bid and ask as decimals of two places
    std::string risk_free_curve;  // the file's metadata risk_free_curve; empty: none
    int64_t row_group_rows = 1'048'576;
};

/** The rows as a backtest day's file. */
auto write_file(const fs::path& path, const std::vector<Row>& rows, Layout layout = {}) -> void {
    arrow::StringBuilder plain;
    arrow::LargeStringBuilder large;
    arrow::Int64Builder ints;
    const auto ts_type = arrow::timestamp(arrow::TimeUnit::MICRO, "America/New_York");
    arrow::TimestampBuilder ts(ts_type, arrow::default_memory_pool());
    arrow::DoubleBuilder bid, ask, ubid, uask;
    const auto cents = arrow::decimal128(9, 2);
    arrow::Decimal128Builder bid_cents(cents), ask_cents(cents);
    arrow::Int64Builder bid_sz, ask_sz;
    for (const Row& r : rows) {
        ASSERT_TRUE((layout.large_symbols ? large.Append(r.symbol) : plain.Append(r.symbol)).ok());
        const int64_t us = kT0Us + (r.minute * kMinuteUs);
        ASSERT_TRUE((layout.ts_as_int ? ints.Append(us) : ts.Append(us)).ok());
        if (layout.decimal_prices) {
            append(bid_cents, r.bid);
            append(ask_cents, 1.6);
        } else {
            append(bid, r.bid);
            append(ask, 1.6);
        }
        append(ubid, r.underlying_bid);
        append(uask, 5000.5);
        ASSERT_TRUE(bid_sz.Append(10).ok());
        ASSERT_TRUE(ask_sz.Append(12).ok());
    }
    const auto symbols = layout.large_symbols ? finish(large) : finish(plain);
    const auto times = layout.ts_as_int ? finish(ints) : finish(ts);
    auto schema = arrow::schema({
        arrow::field("symbol", symbols->type()),
        arrow::field("ts_recv", times->type()),
        arrow::field("bid_px", layout.decimal_prices ? cents : arrow::float64()),
        arrow::field("ask_px", layout.decimal_prices ? cents : arrow::float64()),
        arrow::field("bid_sz", arrow::int64()),
        arrow::field("ask_sz", arrow::int64()),
        arrow::field("underlying_bid_px", arrow::float64()),
        arrow::field("underlying_ask_px", arrow::float64()),
    });
    if (!layout.risk_free_curve.empty()) {
        schema = schema->WithMetadata(
            arrow::key_value_metadata({"risk_free_curve"}, {layout.risk_free_curve}));
    }
    auto table = arrow::Table::Make(
        schema, {symbols, times, layout.decimal_prices ? finish(bid_cents) : finish(bid),
                 layout.decimal_prices ? finish(ask_cents) : finish(ask), finish(bid_sz),
                 finish(ask_sz), finish(ubid), finish(uask)});
    auto out = arrow::io::FileOutputStream::Open(path.string()).ValueOrDie();
    auto props = parquet::ArrowWriterProperties::Builder().store_schema()->build();
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out,
                                           layout.row_group_rows,
                                           parquet::default_writer_properties(), props)
                    .ok());
    ASSERT_TRUE(out->Close().ok());
}

class ParquetQuotesTest : public ::testing::Test {
  protected:
    fs::path dir_ =
        fs::temp_directory_path() /
        ("optra_quotes_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::path file_ = dir_ / "20250804.parquet";
    void SetUp() override { fs::create_directories(dir_); }
    void TearDown() override { fs::remove_all(dir_); }
};

auto rows_per_minute(const adapters::ParquetQuotes& q) -> std::vector<std::size_t> {
    std::vector<std::size_t> out;
    for (const adapters::ParquetQuotes::Minute& m : q.minutes) {
        out.push_back(m.end - m.first);
    }
    return out;
}

TEST_F(ParquetQuotesTest, LargeStringSymbolsAndManyRowGroupsReadEveryRow) {
    write_file(file_, sample_rows(), {.large_symbols = true, .row_group_rows = 1});
    const auto q = adapters::read_parquet_quotes(file_.string());
    ASSERT_TRUE(q) << q.error();
    EXPECT_EQ(q->codes, (std::vector<std::string>{kCall, kPut}));
    EXPECT_EQ(rows_per_minute(*q), (std::vector<std::size_t>{2, 2, 2}));
    EXPECT_EQ(q->rows(), 6U);
}

TEST_F(ParquetQuotesTest, PlainStringSymbolsRead) {
    write_file(file_, sample_rows());
    const auto q = adapters::read_parquet_quotes(file_.string());
    ASSERT_TRUE(q) << q.error();
    EXPECT_EQ(rows_per_minute(*q), (std::vector<std::size_t>{2, 2, 2}));
}

TEST_F(ParquetQuotesTest, RowsComeInTimeOrderAndNullsAreZeroOrNoUnderlying) {
    write_file(file_, {{.minute = 1, .symbol = kPut},
                       {.minute = 0, .symbol = kCall, .bid = std::nullopt},
                       {.minute = 1, .symbol = kCall, .underlying_bid = std::nullopt},
                       {.minute = 0, .symbol = kPut}});
    const auto q = adapters::read_parquet_quotes(file_.string());
    ASSERT_TRUE(q) << q.error();
    ASSERT_EQ(q->minutes.size(), 2U);
    EXPECT_LT(q->minutes[0].time, q->minutes[1].time);
    // Within a time, as written: minute 0 is the call then the put; minute 1 the put then the call.
    const std::vector<std::uint32_t> call_put_put_call{0, 1, 1, 0};
    EXPECT_EQ(q->code, call_put_put_call);
    EXPECT_EQ(q->bid[0], 0.0); // null
    EXPECT_EQ(q->bid[1], 1.5);
    EXPECT_EQ(q->bid_size[0], 10.0);
    EXPECT_TRUE(std::isnan(q->underlying_bid[3])); // null
    EXPECT_EQ(q->underlying_bid[2], 5000.0);
}

TEST_F(ParquetQuotesTest, AColumnOfAnotherTypeIsRefusedWithTheReason) {
    write_file(file_, sample_rows(), {.ts_as_int = true});
    const auto q = adapters::read_parquet_quotes(file_.string());
    ASSERT_FALSE(q);
    EXPECT_NE(q.error().find("ts_recv"), std::string::npos) << q.error();
}

TEST_F(ParquetQuotesTest, ADecimalPriceIsItsIntegerTimesTenToMinusItsPlaces) {
    write_file(file_, {{.minute = 0, .symbol = kCall, .bid = 93.32},
                       {.minute = 0, .symbol = kPut, .bid = std::nullopt}},
               {.decimal_prices = true});
    const auto q = adapters::read_parquet_quotes(file_.string());
    ASSERT_TRUE(q) << q.error();
    EXPECT_EQ(q->bid[0], 9332 * 0.01); // 93.32000000000001, as the data vendors' clients make it
    EXPECT_EQ(q->ask[0], 160 * 0.01);
    EXPECT_EQ(q->bid[1], 0.0); // null
}

TEST_F(ParquetQuotesTest, AMissingFileIsRefusedWithTheReason) {
    const auto q = adapters::read_parquet_quotes((dir_ / "nope.parquet").string());
    ASSERT_FALSE(q);
    EXPECT_FALSE(q.error().empty());
}

} // namespace

TEST_F(ParquetQuotesTest, AChainReadsTheSameFromRowsByContractInRowGroupsOfTheirOwn) {
    std::vector<Row> by_contract;
    for (const char* code : {"250804C05000000", "250804P05000000", "250811C05000000"}) {
        for (int minute = 0; minute < 3; ++minute) {
            by_contract.push_back({.minute = minute, .symbol = code, .bid = 1.0 + minute});
        }
    }
    std::vector<Row> by_time = by_contract;
    std::ranges::stable_sort(by_time, {}, &Row::minute);
    const fs::path a = dir_ / "a" / "SPXW-2025-08" / "20250804.parquet";
    const fs::path b = dir_ / "b" / "SPXW-2025-08" / "20250804.parquet";
    fs::create_directories(a.parent_path());
    fs::create_directories(b.parent_path());
    write_file(a, by_contract, {.row_group_rows = 3}); // a contract a row group
    write_file(b, by_time);
    adapters::HistoricalData x;
    adapters::HistoricalData y;
    x.load(a.string());
    y.load(b.string());
    ASSERT_TRUE(x.has_data() && y.has_data());
    for (adapters::HistoricalData* d : {&x, &y}) {
        d->load_chains(std::array{std::string("SPXW_20250804")});
    }
    EXPECT_EQ(x.loaded_chains(), std::vector<std::string>{"SPXW_20250804"});
    ASSERT_EQ(x.minutes(), y.minutes());
    for (std::size_t i = 0; i < x.minutes(); ++i) {
        model::PortfolioSnapshot f;
        model::PortfolioSnapshot g;
        x.fill(i, f);
        y.fill(i, g);
        EXPECT_EQ(std::vector<double>(f.bid.begin(), f.bid.end()),
                  std::vector<double>(g.bid.begin(), g.bid.end()));
        EXPECT_EQ(std::vector<double>(f.iv.begin(), f.iv.end()),
                  std::vector<double>(g.iv.begin(), g.iv.end()));
        EXPECT_EQ(f.bid[2], 0.0); // the 2025-08-11 chain was not asked for
    }
}

TEST_F(ParquetQuotesTest, ADayIsPricedWithTheCurveItsFileCarriesElseAtTheRateSet) {
    const fs::path with = dir_ / "with" / "SPXW-2025-08" / "20250804.parquet";
    const fs::path without = dir_ / "without" / "SPXW-2025-08" / "20250804.parquet";
    fs::create_directories(with.parent_path());
    fs::create_directories(without.parent_path());
    write_file(with, sample_rows(),
               {.risk_free_curve = R"({"years": [0.08333333333333333, 1.0], "rates": [0.01, 0.04]})"});
    write_file(without, sample_rows());
    const model::RateCurve curve{.years = {1.0 / 12, 1.0}, .rates = {0.01, 0.04}};
    const std::string cache = (dir_ / "cache").string();
    for (const std::string& from : {std::string{}, cache, cache}) { // read, stored, read back
        adapters::HistoricalData d;
        d.set_risk_free_rate(0.03);
        d.load(with.string(), from);
        ASSERT_TRUE(d.has_data()) << d.load_error();
        EXPECT_EQ(d.rates(), curve) << from;
        d.load_all_chains();
        model::PortfolioSnapshot f;
        d.fill(0, f);
        ASSERT_EQ(f.expiry_rate.size(), 1U);
        EXPECT_EQ(f.expiry_rate[0], curve.at(f.expiry_years[0]));
    }
    adapters::HistoricalData d;
    d.set_risk_free_rate(0.03);
    d.load(without.string());
    EXPECT_EQ(d.rates(), model::RateCurve::flat(0.03));
}

TEST_F(ParquetQuotesTest, TheSimulatedMarketAnswersADaysCloseFromThatDaysFile) {
    using namespace std::chrono;
    // Streaming 2025-08-05; asked for 2025-08-04's close, after it was missed.
    const fs::path month = dir_ / "SPY" / "SPY-2025-08";
    fs::create_directories(month);
    std::vector<Row> rows = sample_rows();
    rows.push_back({.minute = 3, .symbol = kCall, .underlying_bid = std::nullopt}); // no quote
    write_file(month / "20250804.parquet", rows);
    write_file(month / "20250805.parquet", sample_rows());
    model::Portfolios portfolios;
    portfolios.ensure_portfolios_created();
    auto source = std::make_unique<adapters::SimulatedMarketData>(
        std::vector<std::string>{(month / "20250805.parquet").string()},
        [] { return model::DateTime{}; }, nullptr);
    core::BookkeepingEngine bookkeeping(nullptr, portfolios);
    const core::StrategyEngine strategies(nullptr, portfolios, bookkeeping, {});
    core::MarketDataEngine engine(nullptr, portfolios, strategies, std::move(source),
                                  [](const model::PortfolioSnapshot&) {});
    std::mutex m;
    std::condition_variable answered;
    std::vector<double> closes;
    engine.on_close([&](const std::string& underlying, year_month_day day, double close) {
        const std::lock_guard lock(m);
        EXPECT_EQ(underlying, "SPY");
        EXPECT_EQ(day, 2025y / August / 4);
        closes.push_back(close);
        answered.notify_all();
    });
    engine.start();
    ASSERT_TRUE(engine.ask_close("SPY", 2025y / August / 4));
    ASSERT_TRUE(engine.ask_close("SPY", 2025y / August / 1)); // no file: unanswered
    std::unique_lock lock(m);
    ASSERT_TRUE(answered.wait_for(lock, seconds(5), [&] { return !closes.empty(); }));
    EXPECT_DOUBLE_EQ(closes.front(), 5000.25); // the last minute quoted both sides: its mid
    answered.wait_for(lock, milliseconds(500), [&] { return closes.size() > 1; });
    EXPECT_EQ(closes.size(), 1U);
    lock.unlock();
    engine.stop();
}

TEST_F(ParquetQuotesTest, TheSimulatedMarketAnswersADaysRangeFromThatDaysFile) {
    using namespace std::chrono;
    const fs::path month = dir_ / "SPY" / "SPY-2025-08";
    fs::create_directories(month);
    const std::vector<Row> rows{{.minute = 0, .symbol = kCall, .underlying_bid = 5000.0},
                                {.minute = 1, .symbol = kCall, .underlying_bid = 5010.0},
                                {.minute = 2, .symbol = kCall, .underlying_bid = 4990.0},
                                {.minute = 3, .symbol = kCall, .underlying_bid = std::nullopt}};
    write_file(month / "20250804.parquet", rows);
    model::Portfolios portfolios;
    portfolios.ensure_portfolios_created();
    auto source = std::make_unique<adapters::SimulatedMarketData>(
        std::vector<std::string>{(month / "20250804.parquet").string()},
        [] { return model::DateTime{}; }, nullptr);
    core::BookkeepingEngine bookkeeping(nullptr, portfolios);
    const core::StrategyEngine strategies(nullptr, portfolios, bookkeeping, {});
    core::MarketDataEngine engine(nullptr, portfolios, strategies, std::move(source),
                                  [](const model::PortfolioSnapshot&) {});
    std::mutex m;
    std::condition_variable answered;
    std::optional<std::array<double, 3>> range;
    engine.on_day_range([&](const std::string& underlying, year_month_day day, double open,
                            double high, double low) {
        const std::lock_guard lock(m);
        EXPECT_EQ(underlying, "SPY");
        EXPECT_EQ(day, 2025y / August / 4);
        range = std::array{open, high, low};
        answered.notify_all();
    });
    engine.start();
    ASSERT_TRUE(engine.ask_day_range("SPY", 2025y / August / 4));
    std::unique_lock lock(m);
    ASSERT_TRUE(answered.wait_for(lock, seconds(5), [&] { return range.has_value(); }));
    // The first minute's mid, the highest and lowest; a minute without the underlying left out.
    EXPECT_EQ(*range, (std::array{5000.25, 5005.25, 4995.25})); // asks 5000.5
    lock.unlock();
    engine.stop();
}
