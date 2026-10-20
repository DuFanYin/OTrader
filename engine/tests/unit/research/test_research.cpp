/**
 * research: the ticks read a contract at a time with their nulls as the row before's, also when a
 * contract runs on into the next row group and when the strikes not kept sit between; trades by
 * contract in time order; limit orders filling on a touch or only through, paying the quote at the
 * window's end when not filled, their cost against the mid at the window's end; option structures
 * screened on backtest days, decided at a bar and valued at the next as a backtest fills, their
 * exits later the same day, the next day, at expiry (settled at their intrinsic value) or not
 * reached; trades placed in the NBBO and summed in cells (bins, the measure's labels, its values;
 * merged), complex trades told by their legs; the order flow, trades signed by the NBBO before
 * them with their Greeks, a cell a day and the big trades a row each, the dealers' gamma by the
 * customers' positions those trades sum to; an option's NBBO at four times of a minute. Writes temporary Parquet files.
 */
#include "research/fills.hpp"
#include "research/offline_data.hpp"
#include "research/structures.hpp"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/util/key_value_metadata.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using research::Contract;
using research::QuoteSeries;
using research::TradeSeries;

struct TickRow {
    double strike;
    int call;
    std::int64_t ms;
    std::optional<std::int64_t> bid; // cents; none: the row before's
    std::optional<std::int64_t> ask;
};

template <typename Builder> auto finish(Builder& b) -> std::shared_ptr<arrow::Array> {
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(b.Finish(&out).ok());
    return out;
}

/** With the schema's metadata kept (price_places), as pyarrow writes it. */
auto write_table(const fs::path& path, const arrow::Table& table, std::int64_t group_rows) -> void {
    auto out = arrow::io::FileOutputStream::Open(path.string()).ValueOrDie();
    auto props = parquet::ArrowWriterProperties::Builder().store_schema()->build();
    ASSERT_TRUE(parquet::arrow::WriteTable(table, arrow::default_memory_pool(), out, group_rows,
                                           parquet::default_writer_properties(), props)
                    .ok());
    ASSERT_TRUE(out->Close().ok());
}

auto write_ticks(const fs::path& path, const std::vector<TickRow>& rows, std::int64_t group_rows)
    -> void {
    arrow::DoubleBuilder strike;
    arrow::Int8Builder call;
    arrow::Int64Builder ms, bid, ask;
    for (const TickRow& r : rows) {
        ASSERT_TRUE(strike.Append(r.strike).ok());
        ASSERT_TRUE(call.Append(static_cast<std::int8_t>(r.call)).ok());
        ASSERT_TRUE(ms.Append(r.ms).ok());
        ASSERT_TRUE((r.bid ? bid.Append(*r.bid) : bid.AppendNull()).ok());
        ASSERT_TRUE((r.ask ? ask.Append(*r.ask) : ask.AppendNull()).ok());
    }
    auto schema = arrow::schema({arrow::field("strike", arrow::float64()),
                                 arrow::field("call", arrow::int8()),
                                 arrow::field("ms", arrow::int64()),
                                 arrow::field("bid", arrow::int64()),
                                 arrow::field("ask", arrow::int64())})
                      ->WithMetadata(arrow::key_value_metadata({"price_places"}, {"2"}));
    auto table = arrow::Table::Make(schema, {finish(strike), finish(call), finish(ms), finish(bid),
                                             finish(ask)});
    write_table(path, *table, group_rows);
}

auto temp(const std::string& name) -> fs::path {
    return fs::temp_directory_path() / ("optra_research_" + name + ".parquet");
}

TEST(ResearchHoldout, ADayInItsPortfoliosHoldoutIsFoundAsTheServerFindsIt) {
    const fs::path home = fs::temp_directory_path() / "optra_research_holdout";
    fs::create_directories(home);
    std::ofstream(home / "config.json")
        << R"({"holdout": {"SPXW": "2025-07-01", "SPY": "2026-04-01"}})";
    ::setenv("OPTRA_HOME", home.c_str(), 1);
    const research::Holdouts h = research::Holdouts::load();
    ::unsetenv("OPTRA_HOME");
    fs::remove_all(home);
    EXPECT_EQ(h.of("SPY"), 20260401);
    EXPECT_EQ(h.of("QQQ"), 20250701); // none of its own: the earliest of the others'
    const std::vector<std::string> before{"data/backtest/SPY/SPY-2026-03/20260331.parquet",
                                          "data/offline/trades/SPXW/SPXW-2025-06/20250630.parquet"};
    EXPECT_EQ(h.first_held(before), std::nullopt);
    const std::vector<std::string> held{"data/backtest/SPY/SPY-2026-03/20260331.parquet",
                                        "data/offline/trades/SPXW/SPXW-2025-07/20250701.parquet"};
    EXPECT_EQ(h.first_held(held), held[1]);
    EXPECT_EQ(research::Holdouts{}.of("SPY"), 0); // no config: no holdout
}

TEST(ResearchTicks, ContractsComeWholeWithTheirNullsFilledAcrossRowGroups) {
    // 100 call: 4 rows over two row groups (3 a group); 101 call (not kept) between; 102 put.
    const std::vector<TickRow> rows{
        {100, 1, 10, 150, 160},
        {100, 1, 20, std::nullopt, 170},
        {100, 1, 30, 155, std::nullopt},
        {100, 1, 40, 155, 170} /* a row group's first row: never null */,
        {101, 1, 10, 50, 60},
        {101, 1, 20, 55, 65},
        {102, 0, 15, 200, 210},
        {102, 0, 5, std::nullopt, 220}};
    const fs::path path = temp("ticks");
    write_ticks(path, rows, 3);
    std::map<Contract, QuoteSeries> got;
    std::vector<Contract> order;
    auto read = research::for_each_contract(
        path.string(), [](double k) { return k != 101.0; },
        [&](const Contract& c, QuoteSeries&& q) {
            order.push_back(c);
            got[c] = std::move(q);
        });
    ASSERT_TRUE(read.has_value()) << read.error();
    ASSERT_EQ(got.size(), 2U);
    const QuoteSeries& c100 = got.at({100.0, true});
    EXPECT_EQ(c100.ms, (std::vector<std::int64_t>{10, 20, 30, 40}));
    EXPECT_EQ(c100.bid, (std::vector<double>{1.50, 1.50, 1.55, 1.55}));
    EXPECT_EQ(c100.ask, (std::vector<double>{1.60, 1.70, 1.70, 1.70}));
    const QuoteSeries& p102 = got.at({102.0, false});
    EXPECT_EQ(p102.ms, (std::vector<std::int64_t>{5, 15})); // put in time order
    EXPECT_EQ(p102.bid, (std::vector<double>{2.00, 2.00}));
    EXPECT_EQ(p102.ask, (std::vector<double>{2.20, 2.10}));
    EXPECT_EQ(order.front(), (Contract{100.0, true})); // handed out once its rows were all read
    fs::remove(path);
}

auto quotes(std::vector<std::int64_t> ms, std::vector<double> bid, std::vector<double> ask)
    -> QuoteSeries {
    return {.ms = std::move(ms), .bid = std::move(bid), .ask = std::move(ask)};
}

TEST(ResearchFills, ABuyAtTheMidFillsOnlyWhenTheAskComesDownToIt) {
    // 1.00 x 1.10 at t0; the ask comes down to 1.05 at 30, the market ends at 1.00 x 1.04.
    const QuoteSeries q = quotes({0, 30, 50}, {1.00, 1.00, 1.00}, {1.10, 1.05, 1.04});
    const std::vector<std::int64_t> t0{10};
    const std::vector<std::int64_t> windows{20, 60};
    const std::vector<double> e{0.0};
    const auto fills = research::simulate_limits(
        q, TradeSeries{}, {.t0_ms = t0, .window_ms = windows, .fill_positions = e});
    ASSERT_EQ(fills.size(), 4U); // 2 windows x buy and sell
    const auto& buy_short = fills[0]; // window 20: ends at 30, the ask's tick at 30 counts
    EXPECT_EQ(buy_short.side, 1);
    EXPECT_TRUE(buy_short.filled_touch);  // the ask reached 1.05
    EXPECT_FALSE(buy_short.filled_through); // and not below it
    // Filled at 1.05 against a mid of 1.025 at the end, half spread 0.05 at t0.
    EXPECT_NEAR(buy_short.cost_touch, (1.05 - 1.025) / 0.05, 1e-9);
    // Not filled through: pays the ask at the end, 1.05.
    EXPECT_NEAR(buy_short.cost_through, (1.05 - 1.025) / 0.05, 1e-9);
    const auto& buy_long = fills[2]; // window 60: the ask at 1.04 goes through 1.05
    EXPECT_TRUE(buy_long.filled_through);
    EXPECT_NEAR(buy_long.cost_through, (1.05 - 1.02) / 0.05, 1e-9);
    EXPECT_NEAR(buy_long.market_cost, (1.10 - 1.02) / 0.05, 1e-9);
    const auto& sell_long = fills[3]; // the bid never rises to 1.05: pays the bid at the end
    EXPECT_EQ(sell_long.side, -1);
    EXPECT_FALSE(sell_long.filled_touch);
    EXPECT_NEAR(sell_long.cost_touch, -(1.00 - 1.02) / 0.05, 1e-9);
}

TEST(ResearchFills, ATradeAtOrThroughTheLimitFillsItAndLimitsRoundAwayFromTheOtherSide) {
    const QuoteSeries q = quotes({0}, {1.00}, {1.07}); // mid 1.035
    TradeSeries trades{.ms = {5, 8}, .price = {1.03, 1.02}};
    const std::vector<std::int64_t> t0{1};
    const std::vector<std::int64_t> windows{5, 10};
    const std::vector<double> e{0.0};
    const auto fills = research::simulate_limits(
        q, trades, {.t0_ms = t0, .window_ms = windows, .fill_positions = e});
    // The buy limit is 1.03 (1.035 down), the sell 1.04 (up).
    const auto& buy5 = fills[0];
    EXPECT_TRUE(buy5.filled_touch);   // a trade at 1.03
    EXPECT_FALSE(buy5.filled_through);
    const auto& buy10 = fills[2];
    EXPECT_TRUE(buy10.filled_through); // a trade at 1.02
    EXPECT_FALSE(fills[3].filled_touch); // no trade at 1.04 or above
}

TEST(ResearchFills, NoOrderWhereTheQuoteIsOneSided) {
    const QuoteSeries q = quotes({0, 100}, {0.0, 1.0}, {0.05, 1.1});
    const std::vector<std::int64_t> t0{10};
    const std::vector<std::int64_t> windows{200};
    const std::vector<double> e{0.0, 0.5};
    EXPECT_TRUE(research::simulate_limits(q, TradeSeries{},
                                          {.t0_ms = t0, .window_ms = windows, .fill_positions = e})
                    .empty());
}

TEST(ResearchTrades, ByContractInTimeOrderOfOneExpiry) {
    arrow::Int32Builder expiry, strike, trade_ms;
    arrow::Int8Builder call;
    arrow::Int64Builder price;
    const std::vector<std::tuple<int, int, int, int, std::int64_t>> rows{
        {20200228, 300000, 1, 2000, 105}, {20200228, 300000, 1, 1000, 100},
        {20200306, 300000, 1, 1500, 900}, {20200228, 299000, 0, 1200, 50}};
    for (const auto& [e, k, c, t, p] : rows) {
        ASSERT_TRUE(expiry.Append(e).ok());
        ASSERT_TRUE(strike.Append(k).ok());
        ASSERT_TRUE(call.Append(static_cast<std::int8_t>(c)).ok());
        ASSERT_TRUE(trade_ms.Append(t).ok());
        ASSERT_TRUE(price.Append(p).ok());
    }
    auto schema = arrow::schema({arrow::field("expiry", arrow::int32()),
                                 arrow::field("strike", arrow::int32()),
                                 arrow::field("call", arrow::int8()),
                                 arrow::field("trade_ms", arrow::int32()),
                                 arrow::field("price", arrow::int64())})
                      ->WithMetadata(arrow::key_value_metadata({"price_places"}, {"2"}));
    auto table = arrow::Table::Make(schema, {finish(expiry), finish(strike), finish(call),
                                             finish(trade_ms), finish(price)});
    const fs::path path = temp("trades");
    write_table(path, *table, 2);
    auto got = research::read_trades(path.string(), 20200228, 1'000'000);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 2U);
    const TradeSeries& c300 = got->at({300.0, true});
    EXPECT_EQ(c300.ms, (std::vector<std::int64_t>{1'001'000, 1'002'000}));
    EXPECT_EQ(c300.price, (std::vector<double>{1.00, 1.05}));
    EXPECT_EQ(got->at({299.0, false}).price, (std::vector<double>{0.50}));
    fs::remove(path);
}

// ---- Structures on backtest days

/** A SPY backtest day of minutes 09:30 to 09:35: the underlying at 600 and options expiring
 * 2025-03-04 at 595, 600 and 605 in parity with it, each a dime wide, every quote up a dime a
 * minute (and `shift` more); 100 contracts open of each. */
auto write_backtest_day(const fs::path& path, std::chrono::year_month_day day, double shift)
    -> void {
    using namespace std::chrono;
    arrow::StringBuilder symbol;
    arrow::TimestampBuilder ts(arrow::timestamp(arrow::TimeUnit::MICRO, "America/New_York"),
                               arrow::default_memory_pool());
    arrow::DoubleBuilder bid, ask, ubid, uask;
    arrow::Int64Builder bid_sz, ask_sz, open_interest;
    // (code, bid at 09:30): calls at 595 and 600, puts at 600 and 605, each its intrinsic + 2
    const std::vector<std::pair<std::string, double>> options{
        {"250304C00595000", 7.0}, {"250304P00595000", 2.0}, {"250304C00600000", 2.0},
        {"250304P00600000", 2.0}, {"250304C00605000", 2.0}, {"250304P00605000", 7.0}};
    const sys_time<microseconds> open = sys_days{day} + 14h + 30min; // 09:30 New York (EST)
    for (int minute = 0; minute <= 5; ++minute) {
        for (const auto& [code, at_open] : options) {
            const double b = at_open + shift + (0.1 * minute);
            ASSERT_TRUE(symbol.Append(code).ok());
            ASSERT_TRUE(ts.Append((open + minutes{minute}).time_since_epoch().count()).ok());
            ASSERT_TRUE(bid.Append(b).ok());
            ASSERT_TRUE(ask.Append(b + 0.1).ok());
            ASSERT_TRUE(ubid.Append(599.99).ok());
            ASSERT_TRUE(uask.Append(600.01).ok());
            ASSERT_TRUE(bid_sz.Append(10).ok());
            ASSERT_TRUE(ask_sz.Append(10).ok());
            ASSERT_TRUE(open_interest.Append(100).ok());
        }
    }
    auto schema = arrow::schema({arrow::field("symbol", arrow::utf8()),
                                 arrow::field("ts_recv", ts.type()),
                                 arrow::field("bid_px", arrow::float64()),
                                 arrow::field("ask_px", arrow::float64()),
                                 arrow::field("bid_sz", arrow::int64()),
                                 arrow::field("ask_sz", arrow::int64()),
                                 arrow::field("underlying_bid_px", arrow::float64()),
                                 arrow::field("underlying_ask_px", arrow::float64()),
                                 arrow::field("open_interest", arrow::int64())});
    auto table = arrow::Table::Make(schema, {finish(symbol), finish(ts), finish(bid), finish(ask),
                                             finish(bid_sz), finish(ask_sz), finish(ubid),
                                             finish(uask), finish(open_interest)});
    fs::create_directories(path.parent_path());
    write_table(path, *table, 1 << 16);
}

TEST(ResearchStructures, DecidedAtABarValuedAtTheNextExitsSameDayNextDayAtExpiryOrNotReached) {
    using namespace std::chrono;
    const fs::path root = fs::temp_directory_path() / "optra_research_screen" / "SPY";
    const fs::path monday = root / "SPY-2025-03" / "20250303.parquet";
    const fs::path tuesday = root / "SPY-2025-03" / "20250304.parquet";
    write_backtest_day(monday, 2025y / March / 3, 0.0);
    write_backtest_day(tuesday, 2025y / March / 4, 0.5);
    research::Structure call{
        .name = "ATM call",
        .legs = {{.rule = {.ratio = 1, .right = model::OptionType::CALL}}},
        .entry_minutes = {1},
        .exits = {{.label = "same day", .at = research::StructureExit::At::kDaysAfter, .days = 0,
                   .minute = 3},
                  {.label = "next day", .at = research::StructureExit::At::kDaysAfter, .days = 1,
                   .minute = 1},
                  {.label = "expiry", .at = research::StructureExit::At::kExpiry}}};
    const research::ScreenSpec spec{.days = {monday.string(), tuesday.string()},
                                    .structures = {call}};
    research::StructureTable table(spec.structures);
    auto errors = research::screen_structures(spec, table);
    ASSERT_TRUE(errors.has_value()) << errors.error();
    EXPECT_TRUE(errors->empty());
    ASSERT_EQ(table.rows(), 6U); // two entries x three exits
    const fs::path out = temp("screen");
    ASSERT_TRUE(table.write(out.string()).has_value());

    auto reader = parquet::arrow::OpenFile(arrow::io::ReadableFile::Open(out.string()).ValueOrDie(),
                                           arrow::default_memory_pool());
    ASSERT_TRUE(reader.ok());
    std::shared_ptr<arrow::Table> rows;
    ASSERT_TRUE((*reader)->ReadTable(&rows).ok());
    const auto col = [&](const char* name) {
        return std::static_pointer_cast<arrow::DoubleArray>(rows->GetColumnByName(name)->chunk(0));
    };
    const auto ints = [&](const char* name) {
        return std::static_pointer_cast<arrow::Int32Array>(rows->GetColumnByName(name)->chunk(0));
    };
    // Monday's entry: the 600 call picked at minute 1, bought at minute 2's ask 2.3 (2.2 x 2.3).
    EXPECT_EQ(ints("entry_day")->Value(0), 20250303);
    EXPECT_EQ(ints("entry_minute")->Value(0), 1);
    EXPECT_NEAR(col("entry_quotes")->Value(0), 230.0, 1e-9);
    EXPECT_NEAR(col("entry_mid")->Value(0), 225.0, 1e-9);
    // Same day: decided at minute 3, sold at minute 4's bid 2.4.
    EXPECT_NEAR(col("exit_quotes")->Value(0), 240.0, 1e-9);
    EXPECT_EQ(ints("exit_minute")->Value(0), 3);
    // Next day: decided at minute 1, sold at minute 2's bid, Tuesday's quotes half a dollar up.
    EXPECT_EQ(ints("exit_day")->Value(1), 20250304);
    EXPECT_NEAR(col("exit_quotes")->Value(1), (2.5 + 0.2) * 100.0, 1e-9);
    // At expiry: settled at the intrinsic value at the underlying's last mid, 600: nothing.
    EXPECT_EQ(ints("exit_day")->Value(2), 20250304);
    EXPECT_NEAR(col("exit_mid")->Value(2), 0.0, 1e-9);
    // Tuesday's entry: its next day has no file, not reached (the day it was due kept); its
    // expiry is the same day.
    EXPECT_EQ(ints("entry_day")->Value(3), 20250304);
    EXPECT_TRUE(std::isnan(col("exit_mid")->Value(4)));
    EXPECT_EQ(ints("exit_day")->Value(4), 20250305);
    EXPECT_NEAR(col("exit_mid")->Value(5), 0.0, 1e-9);
    fs::remove_all(root.parent_path());
    fs::remove(out);
}

/** A cells file's rows: its key columns joined by "|", its sums. */
auto read_cells(const fs::path& path, std::size_t keys)
    -> std::map<std::string, std::vector<double>> {
    auto reader = parquet::arrow::OpenFile(
        arrow::io::ReadableFile::Open(path.string()).ValueOrDie(), arrow::default_memory_pool());
    std::shared_ptr<arrow::Table> t;
    EXPECT_TRUE(reader.ok() && (*reader)->ReadTable(&t).ok());
    std::map<std::string, std::vector<double>> out;
    for (std::int64_t r = 0; r < t->num_rows(); ++r) {
        std::string key;
        for (std::size_t k = 0; k < keys; ++k) {
            const auto c = std::static_pointer_cast<arrow::StringArray>(
                t->column(static_cast<int>(k))->chunk(0));
            key += (k > 0 ? "|" : "") +
                   (c->IsNull(r) ? std::string("null") : std::string(c->GetView(r)));
        }
        for (int c = static_cast<int>(keys); c < t->num_columns(); ++c) {
            out[key].push_back(
                std::static_pointer_cast<arrow::DoubleArray>(t->column(c)->chunk(0))->Value(r));
        }
    }
    return out;
}

TEST(ResearchCells, BinsLeftClosedLabelsAndValuesNullsAndMerging) {
    const auto make = [] {
        return research::TradeCells(
            {{.name = "b", .measure = "x", .edges = {1, 2}, .labels = {"<1", "1-2", ">=2"}},
             {.name = "k", .measure = "kind"},
             {.name = "y", .measure = "year"}},
            {{"x", {}}, {"kind", {"a", "b"}}, {"year", {}}}, {"n", "v"});
    };
    research::TradeCells one = make();
    research::TradeCells two = make();
    const std::array<double, 3> at_edge{1.0, 0.0, 2024.0}; // 1 opens the second bin
    one.add(at_edge, std::array{1.0, 2.0});
    one.add(at_edge, std::array{1.0, std::nan("")}); // a NaN value adds nothing
    two.add(std::array{std::nan(""), 1.0, 2025.0}, std::array{1.0, 5.0});
    two.add(at_edge, std::array{1.0, 3.0});
    one.merge(two);
    const fs::path out = temp("cells");
    ASSERT_TRUE(one.write(out.string()).has_value());
    const auto cells = read_cells(out, 3);
    ASSERT_EQ(cells.size(), 2U);
    EXPECT_EQ(cells.at("1-2|a|2024"), (std::vector<double>{3.0, 5.0}));
    EXPECT_EQ(cells.at("null|b|2025"), (std::vector<double>{1.0, 5.0}));
    EXPECT_THROW((research::TradeCells({{.name = "z", .measure = "nope"}}, {{"x", {}}}, {"n"})),
                 std::invalid_argument);
    fs::remove(out);
}

TEST(ResearchCombos, LegsOfAMillisecondExchangeAndConditionToldByTheirContracts) {
    using namespace std::chrono;
    research::TradeRows t;
    const auto row = [&](std::int32_t ms, double strike, bool call, std::int32_t size, double price,
                         double bid, double ask, int expiry = 20250303) {
        t.expiry.push_back(expiry);
        t.strike.push_back(strike);
        t.call.push_back(call ? 1 : 0);
        t.ms.push_back(ms);
        t.condition.push_back(130);
        t.exchange.push_back(1);
        t.price.push_back(price);
        t.bid.push_back(bid);
        t.ask.push_back(ask);
        t.size.push_back(size);
    };
    // An iron butterfly (the puts' top strike the calls' bottom), at the legs' mids: e 0.
    row(1000, 95, false, 2, 1.0, 0.9, 1.1);
    row(1000, 100, false, 2, 3.0, 2.9, 3.1);
    row(1000, 100, true, 2, 3.0, 2.9, 3.1);
    row(1000, 105, true, 2, 1.0, 0.9, 1.1);
    // A 1:2:1 call butterfly filled where legging in would have: e 1 (wings bought at the ask,
    // the body sold at the bid).
    row(2000, 100, true, 1, 3.2, 3.0, 3.2);
    row(2000, 105, true, 2, 1.9, 1.9, 2.1);
    row(2000, 110, true, 1, 1.2, 1.0, 1.2);
    // Three strikes 1:1:1: no structure; two expiries, one strike: a calendar; two expiries, two
    // strikes: left out.
    row(3000, 100, true, 1, 3.0, 2.9, 3.1);
    row(3000, 105, true, 1, 2.0, 1.9, 2.1);
    row(3000, 110, true, 1, 1.0, 0.9, 1.1);
    row(4000, 100, true, 1, 3.0, 2.9, 3.1);
    row(4000, 100, true, 1, 3.5, 3.4, 3.6, 20250304);
    row(4500, 100, true, 1, 3.0, 2.9, 3.1);
    row(4500, 105, true, 1, 3.5, 3.4, 3.6, 20250304);
    // A straddle, both legs bought at the ask: e 1.
    row(5000, 100, false, 3, 2.1, 1.9, 2.1);
    row(5000, 100, true, 3, 3.1, 2.9, 3.1);
    research::TradeCells cells = research::combo_cells({{.name = "structure"}});
    research::place_combos(t, 2025y / March / 3, cells);
    const fs::path out = temp("combos");
    ASSERT_TRUE(cells.write(out.string()).has_value());
    const auto got = read_cells(out, 1);
    ASSERT_EQ(got.size(), 4U);
    EXPECT_NEAR(got.at("calendar")[2], 0.0, 1e-9);
    EXPECT_EQ(got.at("straddle")[1], 3.0);
    EXPECT_NEAR(got.at("straddle")[2], 1.0, 1e-9);
    // trades, units, e_sum, e_units_sum, at_mid, within_half, legged_or_worse, legged_usd, saved
    const std::vector<double>& fly = got.at("iron condor or fly");
    EXPECT_EQ(fly[0], 1.0);
    EXPECT_EQ(fly[1], 2.0);
    EXPECT_NEAR(fly[2], 0.0, 1e-12);
    EXPECT_EQ(fly[4], 1.0);
    EXPECT_NEAR(fly[7], 0.4 * 2, 1e-12); // four legs of half spread 0.1, two units
    const std::vector<double>& butterfly = got.at("butterfly");
    EXPECT_NEAR(butterfly[2], 1.0, 1e-9);
    EXPECT_EQ(butterfly[6], 1.0);
    EXPECT_NEAR(butterfly[7], 0.4, 1e-12); // 0.1 + 2 x 0.1 + 0.1
    fs::remove(out);
}

TEST(ResearchFlow, TradesSignedByTheMidWithTheirGreeksInCellsAndTheBigOnesARowEach) {
    using namespace std::chrono;
    research::TradeRows t;
    const auto row = [&](bool call, std::int32_t size, double price, std::int16_t condition = 18) {
        t.expiry.push_back(20250303);
        t.strike.push_back(100.0);
        t.call.push_back(call ? 1 : 0);
        t.ms.push_back(((10 * 60) * 60'000) + 30'000); // 10:00:30: the third 15 minutes
        t.condition.push_back(condition);
        t.exchange.push_back(1);
        t.price.push_back(price);
        t.bid.push_back(0.9);
        t.ask.push_back(1.1);
        t.size.push_back(size);
    };
    row(true, 2, 1.1);      // bought at the ask
    row(true, 1, 0.9);      // sold at the bid
    row(false, 3, 1.0);     // at the mid: not signed
    row(true, 1000, 1.1);   // big, bought
    row(true, 5, 1.1, 130); // a multi-leg condition: its own cell, never big
    research::FlowTable flow(1000);
    const std::vector<int> minutes{599, 600};
    const std::vector<double> underlying{100.0, 100.0};
    flow.add(t, 2025y / March / 3, minutes, underlying);
    ASSERT_EQ(flow.rows(), 4U);
    ASSERT_EQ(flow.big_rows(), 1U);
    const fs::path out = temp("flow");
    const fs::path big = temp("flow_big");
    ASSERT_TRUE(flow.write(out.string()).has_value());
    ASSERT_TRUE(flow.write_big(big.string()).has_value());
    const auto read = [](const fs::path& path) {
        auto reader =
            parquet::arrow::OpenFile(arrow::io::ReadableFile::Open(path.string()).ValueOrDie(),
                                     arrow::default_memory_pool());
        std::shared_ptr<arrow::Table> table;
        EXPECT_TRUE(reader.ok() && (*reader)->ReadTable(&table).ok());
        return table;
    };
    const auto cells = read(out);
    // by bin|dte|call|multi|size: trades, contracts, buy, sell, delta_shares, abs_delta_shares, ..
    std::map<std::string, std::vector<double>> got;
    for (std::int64_t r = 0; r < cells->num_rows(); ++r) {
        std::string key;
        for (int c = 1; c <= 5; ++c) {
            key += (c > 1 ? "|" : "") +
                   std::to_string(std::static_pointer_cast<arrow::Int8Array>(
                                      cells->column(c)->chunk(0))->Value(r));
        }
        for (int c = 6; c < cells->num_columns(); ++c) {
            got[key].push_back(
                std::static_pointer_cast<arrow::DoubleArray>(cells->column(c)->chunk(0))->Value(r));
        }
    }
    const std::vector<double>& small_calls = got.at("2|0|1|0|0");
    EXPECT_EQ(small_calls[0], 2.0);
    EXPECT_EQ(small_calls[1], 3.0);
    EXPECT_EQ(small_calls[2], 2.0);
    EXPECT_EQ(small_calls[3], 1.0);
    const double delta = small_calls[5] / 300.0; // one minute's Greeks for the contract
    EXPECT_GT(delta, 0.4);
    EXPECT_LT(delta, 0.6);
    EXPECT_NEAR(small_calls[4], 100.0 * delta, 1e-9); // 2 bought less 1 sold
    EXPECT_GT(small_calls[6], 0.0);                    // customers bought gamma
    const std::vector<double>& puts = got.at("2|0|0|0|0");
    EXPECT_EQ(puts[2] + puts[3], 0.0);
    EXPECT_EQ(puts[4], 0.0);
    EXPECT_GT(puts[5], 0.0);
    EXPECT_EQ(got.at("2|0|1|0|2")[2], 1000.0);
    EXPECT_EQ(got.at("2|0|1|1|0")[1], 5.0);
    const auto rows = read(big);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int8Array>(rows->GetColumnByName("sign")->chunk(0))
                  ->Value(0),
              1);
    const auto deltas =
        std::static_pointer_cast<arrow::DoubleArray>(rows->GetColumnByName("delta")->chunk(0));
    EXPECT_NEAR(deltas->Value(0), delta, 1e-12);
    fs::remove(out);
    fs::remove(big);
}

TEST(ResearchGamma, TheCustomersPositionsAreTheirNetsOfTheDaysBeforeTheDealersTheOtherSide) {
    using namespace std::chrono;
    const fs::path root = fs::temp_directory_path() / "optra_research_gamma" / "SPY";
    const fs::path monday = root / "SPY-2025-03" / "20250303.parquet";
    const fs::path tuesday = root / "SPY-2025-03" / "20250304.parquet";
    write_backtest_day(monday, 2025y / March / 3, 0.0);
    write_backtest_day(tuesday, 2025y / March / 4, 0.0);
    // Monday's flow: customers bought 10 of the 600 call net, sold 4 of the 600 put.
    research::TradeRows t;
    const auto row = [&](bool call, std::int32_t size, double price) {
        t.expiry.push_back(20250304);
        t.strike.push_back(600.0);
        t.call.push_back(call ? 1 : 0);
        t.ms.push_back(((10 * 60) * 60'000));
        t.condition.push_back(18);
        t.exchange.push_back(1);
        t.price.push_back(price);
        t.bid.push_back(2.0);
        t.ask.push_back(2.1);
        t.size.push_back(size);
    };
    row(true, 12, 2.1);
    row(true, 2, 2.0);
    row(false, 4, 2.0);
    research::FlowTable flow(1000);
    const std::vector<int> minutes{599};
    const std::vector<double> underlying{600.0};
    flow.add(t, 2025y / March / 3, minutes, underlying);
    const fs::path nets = temp("contract_nets");
    ASSERT_TRUE(flow.write_contracts(nets.string()).has_value());
    const auto read = research::read_contract_nets(nets.string());
    ASSERT_TRUE(read.has_value()) << read.error();
    ASSERT_EQ(read->size(), 2U);

    research::GammaTable table;
    const auto errors = research::measure_gamma(
        {.days = {monday.string(), tuesday.string()}, .minutes = {1}, .contract_nets = nets.string()},
        table);
    ASSERT_TRUE(errors.has_value()) << errors.error();
    EXPECT_TRUE(errors->empty());
    const fs::path out = temp("gamma");
    ASSERT_TRUE(table.write(out.string()).has_value());
    auto reader = parquet::arrow::OpenFile(arrow::io::ReadableFile::Open(out.string()).ValueOrDie(),
                                           arrow::default_memory_pool());
    std::shared_ptr<arrow::Table> rows;
    ASSERT_TRUE(reader.ok() && (*reader)->ReadTable(&rows).ok());
    ASSERT_EQ(rows->num_rows(), 2);
    const auto col = [&](const char* name) {
        return std::static_pointer_cast<arrow::DoubleArray>(rows->GetColumnByName(name)->chunk(0));
    };
    EXPECT_EQ(col("flow_contracts")->Value(0), 0.0); // Monday: no day before it
    EXPECT_EQ(col("gex_flow")->Value(0), 0.0);
    EXPECT_EQ(col("flow_contracts")->Value(1), 14.0); // Tuesday: +10 calls, -4 puts
    // The dealers short the 10 calls customers bought, long the 4 puts they sold: 6 contracts'
    // gamma short (one strike, the same gamma a call and a put), all of it expiring.
    EXPECT_LT(col("gex_flow")->Value(1), 0.0);
    EXPECT_DOUBLE_EQ(col("gex_flow_0dte")->Value(1), col("gex_flow")->Value(1));
    fs::remove(nets);
    fs::remove(out);
    fs::remove_all(root.parent_path());
}

TEST(ResearchIntraminute, TheQuoteInForceAtFourTimesWhereAllAreTwoSided) {
    QuoteSeries q{.ms = {0, 30'000, 61'000, 125'000}, .bid = {1.0, 1.1, 1.2, 0.0},
                  .ask = {1.2, 1.3, 1.4, 1.5}};
    research::IntraminuteRows rows;
    research::sample_intraminute(q, std::vector<std::int64_t>{0, 60'000, 120'000}, 7, rows);
    // Minute 0: m 1.0/1.2, m + 1 s the same, m + 60 s 1.1/1.3 (the tick at 30 s); minute 1: m
    // 1.1/1.3, m + 1 s 1.2/1.4; minute 2's next quote (180 s) is one-sided: not kept.
    ASSERT_EQ(rows.minute.size(), 2U);
    EXPECT_EQ(rows.minute[0], 0);
    EXPECT_FLOAT_EQ(rows.mid_m[0], 1.1F);
    EXPECT_FLOAT_EQ(rows.bid_d[0], 1.0F);
    EXPECT_FLOAT_EQ(rows.ask_n[0], 1.3F);
    EXPECT_EQ(rows.minute[1], 1);
    EXPECT_FLOAT_EQ(rows.mid_d[1], 1.3F);
}

} // namespace
