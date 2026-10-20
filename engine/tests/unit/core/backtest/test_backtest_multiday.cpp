/**
 * A backtest over several days runs them as the live engine trades them: each day an engine of
 * its own, restored from the storage the days share, so what is held at a close is held the next
 * morning; the options expiring at a close settle there (SPY's in shares). A day's P&L is the
 * change since the close before, the overnight move included, and the days add up to the run. A
 * run stops where a day would be wrong: a trading day missing, or a contract held that the next
 * day does not quote, while something is held. And the exchange's calendar: holidays, early closes.
 * A strategy made again during the day, as a restart makes it, trades as if it had run through.
 *
 * Writes small SPY days as Parquet (data/backtest/SPY/SPY-YYYY-MM/YYYYMMDD.parquet's layout) in a temporary
 * directory.
 */
#include "tests/lone_backtest.hpp"
#include "adapters/market_data/historical_data.hpp"
#include "model/session_calendar.hpp"
#include "model/daily_table.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "pricing/pricing.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include <algorithm>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <numeric>
#include <parquet/arrow/writer.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono;

/**
 * Buys one call at the open of buy_day (YYYYMMDD) and sells it two minutes later on sell_day;
 * holds it in between. Created each morning, like every strategy in a run: it knows what it holds only from
 * its holding.
 */
class OvernightProbe final : public strategy::Strategy {
  public:
    struct Setting {
        strategy::Param<int> buy_day{"buy_day", 0, 0, 99'999'999};
        strategy::Param<int> sell_day{"sell_day", 0, 0, 99'999'999};
        strategy::Param<int> expiry{"expiry", 0, 0, 99'999'999};
        strategy::Param<double> strike{"strike", 100, 1, 10'000};
    };
    using Strategy::Strategy;

    void on_timer() override {
        const year_month_day today = model::kNyse.date_of(now());
        const int day = static_cast<int>(today.year()) * 10'000 +
                        static_cast<int>(static_cast<unsigned>(today.month())) * 100 +
                        static_cast<int>(static_cast<unsigned>(today.day()));
        const int minute = model::kNyse.minutes_since_open(now());
        if (has_active_order()) {
            return;
        }
        const bool flat = holding() == nullptr || holding()->is_flat();
        if (day == setting_.buy_day && minute == 0 && flat) {
            const int e = setting_.expiry;
            const std::string symbol = model::option_symbol(
                {.root = "SPY",
                 .expiry = year_month_day{year{e / 10'000}, month{static_cast<unsigned>(e / 100 % 100)},
                                          std::chrono::day{static_cast<unsigned>(e % 100)}},
                 .type = model::OptionType::CALL,
                 .strike = setting_.strike});
            ASSERT_TRUE(portfolio()->option(symbol)) << symbol;
            send_order(std::array{leg(symbol, model::Side::BUY)});
        } else if (day == setting_.sell_day && minute == 2 && !flat) {
            close_all_strategy_positions();
        }
    }

  private:
    const Setting setting_ = read_setting<Setting>();
};

REGISTER_STRATEGY(OvernightProbe);

/** Buys the weekly call at a limit at limit_minute, and at the market at market_minute. */
class LateOrderProbe final : public strategy::Strategy {
  public:
    struct Setting {
        strategy::Param<int> limit_minute{"limit_minute", -1, -1, 390};
        strategy::Param<int> market_minute{"market_minute", -1, -1, 390};
    };
    using Strategy::Strategy;

    void on_timer() override {
        const std::string call = "SPY-20250307-C-100";
        ASSERT_TRUE(portfolio()->option(call));
        if (minute() == setting_.limit_minute) {
            send_order(std::array{leg(call, model::Side::BUY)}, 0.01, 1.0,
                       model::OrderType::LIMIT);
        }
        if (minute() == setting_.market_minute) {
            send_order(std::array{leg(call, model::Side::BUY)});
        }
    }

  private:
    const Setting setting_ = read_setting<Setting>();
};

REGISTER_STRATEGY(LateOrderProbe);

/** Fails at the first minute after the open. */
class FailingProbe final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_timer() override {
        if (minute() == 1) {
            set_error("gave up");
        }
    }
};

REGISTER_STRATEGY(FailingProbe);

/** Buys the weekly call from on_snapshot at the open's bar, its chain quoted from the start: the
 * quote it reacts to. */
class SnapshotOrderProbe final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_day() override {
        const std::array chain{std::string("SPY_20250307")};
        subscribe_chains(chain);
    }
    void on_snapshot() override {
        const bool flat = holding() == nullptr || holding()->is_flat();
        if (minute() == 0 && flat && !has_active_order()) {
            send_order(std::array{leg("SPY-20250307-C-100", model::Side::BUY)});
        }
    }
    void on_timer() override {}
};

REGISTER_STRATEGY(SnapshotOrderProbe);

/** Trades nothing; writes the bars it saw today and the minute of the last. */
class MeasuringProbe final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_day() override { bars_ = 0; }
    void on_timer() override {
        write_value("bars", ++bars_);
        write_value("minute", minute());
    }

  private:
    int bars_ = 0;
};

REGISTER_STRATEGY(MeasuringProbe);

/** One option's quote through a day: its bid and ask at every bar. */
struct Quote {
    std::string occ; // 250307C00100000
    double bid;
    double ask;
};

/** A bar: minutes from the 09:30 open, the underlying's mid, the options' quotes. */
struct Bar {
    int minute;
    double spot;
    std::vector<Quote> options;
};

class MultiDayBacktest : public ::testing::Test {
  protected:
    fs::path dir_ = fs::temp_directory_path() /
                    std::format("optra_multiday_{}_{}",
                                ::testing::UnitTest::GetInstance()->random_seed(),
                                ::testing::UnitTest::GetInstance()->current_test_info()->name());
    void SetUp() override { fs::create_directories(dir_); }
    void TearDown() override { fs::remove_all(dir_); }

    /** Writes the day's bars as a backtest day of SPY; its path. */
    auto write_day(year_month_day day, const std::vector<Bar>& bars) -> std::string {
        const fs::path folder =
            dir_ / std::format("SPY-{:%Y-%m}", sys_days{day});
        fs::create_directories(folder);
        const fs::path path = folder / std::format("{:%Y%m%d}.parquet", sys_days{day});

        arrow::StringBuilder symbol;
        const auto ts_type = arrow::timestamp(arrow::TimeUnit::MICRO, "America/New_York");
        arrow::TimestampBuilder ts(ts_type, arrow::default_memory_pool());
        arrow::DoubleBuilder bid, ask, ubid, uask;
        arrow::Int64Builder bid_sz, ask_sz, ubid_sz, uask_sz;
        const auto* ny = locate_zone("America/New_York");
        for (const Bar& bar : bars) {
            const auto at = zoned_time{ny, local_days{day} + 9h + 30min + minutes{bar.minute}}
                                .get_sys_time();
            const int64_t us = duration_cast<microseconds>(at.time_since_epoch()).count();
            for (const Quote& q : bar.options) {
                EXPECT_TRUE(symbol.Append(q.occ).ok());
                EXPECT_TRUE(ts.Append(us).ok());
                EXPECT_TRUE(bid.Append(q.bid).ok());
                EXPECT_TRUE(ask.Append(q.ask).ok());
                EXPECT_TRUE(ubid.Append(bar.spot - 0.01).ok());
                EXPECT_TRUE(uask.Append(bar.spot + 0.01).ok());
                for (arrow::Int64Builder* size : {&bid_sz, &ask_sz, &ubid_sz, &uask_sz}) {
                    EXPECT_TRUE(size->Append(100).ok());
                }
            }
        }
        auto schema = arrow::schema({
            arrow::field("symbol", arrow::utf8()),
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
        auto table = arrow::Table::Make(
            schema, {symbol.Finish().ValueOrDie(), ts.Finish().ValueOrDie(),
                     bid.Finish().ValueOrDie(), ask.Finish().ValueOrDie(),
                     bid_sz.Finish().ValueOrDie(), ask_sz.Finish().ValueOrDie(),
                     ubid.Finish().ValueOrDie(), uask.Finish().ValueOrDie(),
                     ubid_sz.Finish().ValueOrDie(), uask_sz.Finish().ValueOrDie()});
        auto out = arrow::io::FileOutputStream::Open(path.string()).ValueOrDie();
        EXPECT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 1 << 16)
                        .ok());
        EXPECT_TRUE(out->Close().ok());
        return path.string();
    }
};

// 2025-03-03 to 03-05, a Monday to a Wednesday; the call expiring on the Friday.
const year_month_day kMon = 2025y / March / 3;
const year_month_day kTue = 2025y / March / 4;
const year_month_day kWed = 2025y / March / 5;
constexpr const char* kWeekly = "250307C00100000"; // SPY 2025-03-07 call 100
constexpr const char* kDaily = "250303C00100000";  // SPY 2025-03-03 call 100: expires on Monday

/** A day of bars at the open (minutes 0 to 3) and at the 16:00 close, each option quoted 0.05
 * either side of its intrinsic value plus one. */
auto day_bars(double spot, std::initializer_list<const char*> options = {kWeekly},
              std::initializer_list<int> minutes = {0, 1, 2, 3, 390}) -> std::vector<Bar> {
    std::vector<Bar> bars;
    for (const int minute : minutes) {
        Bar bar{.minute = minute, .spot = spot, .options = {}};
        for (const char* occ : options) {
            const double value = std::max(spot - 100.0, 0.0) + 1.0;
            bar.options.push_back({occ, value - 0.05, value + 0.05});
        }
        bars.push_back(std::move(bar));
    }
    return bars;
}

auto run(const std::vector<std::string>& files, int buy_day, int sell_day, int expiry = 20250307)
    -> backtest::BacktestRunSummary {
    const backtest::BacktestTrial trial{
        .strategy_name = "OvernightProbe",
        .strategy_setting =
            {{"buy_day", buy_day}, {"sell_day", sell_day}, {"expiry", expiry}, {"strike", 100}},
        .execution = {},
        .fills = true};
    return std::move(backtest::run_backtests(files, std::span(&trial, 1),
                                                   /*risk_free_rate=*/0.0, "mid",
                                                   utilities::DISABLED)
                         .front());
}

/** The run's P&L at the close of the last day it ran. */
auto final_pnl(const backtest::BacktestRunSummary& s) -> double {
    for (auto day = s.daily_results.rbegin(); day != s.daily_results.rend(); ++day) {
        if (!day->load_failed) {
            return day->final_pnl;
        }
    }
    return 0.0;
}

auto summed(const backtest::BacktestRunSummary& s) -> double {
    return std::accumulate(s.daily_results.begin(), s.daily_results.end(), 0.0,
                           [](double sum, const auto& d) { return sum + d.daily_pnl; });
}

TEST_F(MultiDayBacktest, APositionHeldAtTheCloseIsHeldTheNextMorning) {
    // Bought on Monday at the ask a bar after the open (1.05 at spot 100), marked at Monday's close
    // (mid 1.00), sold on Tuesday at the bid with the underlying 2 higher (2.95).
    const auto s = run({write_day(kMon, day_bars(100.0)), write_day(kTue, day_bars(102.0))},
                       20250303, 20250304);
    ASSERT_EQ(s.daily_results.size(), 2U);
    EXPECT_TRUE(s.errors.empty()) << s.errors.front();
    const auto& mon = s.daily_results[0];
    const auto& tue = s.daily_results[1];
    EXPECT_FALSE(mon.flat_at_close);
    EXPECT_TRUE(tue.flat_at_close);
    EXPECT_NEAR(mon.daily_pnl, (1.00 - 1.05) * 100.0, 1e-6);
    EXPECT_NEAR(tue.daily_pnl, (2.95 - 1.00) * 100.0, 1e-6); // the overnight move is Tuesday's
    EXPECT_NEAR(final_pnl(s), (2.95 - 1.05) * 100.0, 1e-6);
    EXPECT_NEAR(summed(s), final_pnl(s), 1e-6);
    // Each day reports its own fills, under ids no other day uses.
    ASSERT_EQ(mon.fills.size(), 1U);
    ASSERT_EQ(tue.fills.size(), 1U);
    EXPECT_NE(mon.fills[0].order_id, tue.fills[0].order_id);
    EXPECT_EQ(tue.total_orders, 1);
}

TEST_F(MultiDayBacktest, AnOrderSentFromOnSnapshotFillsAtTheNextBarsQuote) {
    // The open's ask 1.05 (spot 100), the next bar's 2.05 (spot 101): what on_snapshot sends on
    // seeing a quote fills at the bar after, as one from on_timer does, never at the quote seen.
    std::vector<Bar> bars = day_bars(100.0, {kWeekly}, {0});
    for (Bar& bar : day_bars(101.0, {kWeekly}, {1, 2, 3, 390})) {
        bars.push_back(std::move(bar));
    }
    const backtest::BacktestTrial trial{.strategy_name = "SnapshotOrderProbe",
                                        .execution = {},
                                        .fills = true};
    const auto s = backtest::run_backtests({write_day(kMon, bars)}, std::span(&trial, 1),
                                           /*risk_free_rate=*/0.0, "mid", utilities::DISABLED)
                       .front();
    ASSERT_EQ(s.daily_results.size(), 1U);
    ASSERT_EQ(s.daily_results[0].fills.size(), 1U);
    EXPECT_DOUBLE_EQ(s.daily_results[0].fills[0].price, 2.05);
}

TEST_F(MultiDayBacktest, ATrialAskingForItsLogKeepsEachDaysRecordsAtTheDatasTime) {
    const std::vector<std::string> files = {write_day(kMon, day_bars(100.0)),
                                            write_day(kTue, day_bars(102.0))};
    backtest::BacktestTrial trial{
        .strategy_name = "OvernightProbe",
        .strategy_setting =
            {{"buy_day", 20250303}, {"sell_day", 20250304}, {"expiry", 20250307}, {"strike", 100}},
        .execution = {},
        .log = true};
    const auto s = backtest::run_backtests(files, std::span(&trial, 1), 0.0, "mid",
                                                 utilities::DISABLED)
                       .front();
    ASSERT_EQ(s.daily_results.size(), 2U);
    for (const auto& [day, date] : {std::pair{0, kMon}, std::pair{1, kTue}}) {
        const auto& log = s.daily_results[day].log;
        const auto fill = std::ranges::find_if(
            log, [](const utilities::LogRecord& r) { return r.msg.starts_with("Fill "); });
        ASSERT_NE(fill, log.end()) << "day " << day;
        EXPECT_EQ(year_month_day{floor<days>(fill->at)}, date); // the data's day, not today
    }
    trial.log = false;
    const auto quiet = backtest::run_backtests(files, std::span(&trial, 1), 0.0, "mid",
                                                     utilities::DISABLED)
                           .front();
    EXPECT_TRUE(quiet.daily_results[0].log.empty());
}

TEST_F(MultiDayBacktest, EachDayReportsTheValuesItsStrategyWroteLast) {
    const std::vector<std::string> files = {write_day(kMon, day_bars(100.0)),
                                            write_day(kTue, day_bars(102.0))};
    const backtest::BacktestTrial trial{.strategy_name = "MeasuringProbe"};
    const auto s = backtest::run_backtests(files, std::span(&trial, 1), 0.0, "mid",
                                           utilities::DISABLED)
                       .front();
    ASSERT_EQ(s.daily_results.size(), 2U);
    for (const auto& day : s.daily_results) {
        ASSERT_EQ(day.values.size(), 2U);
        EXPECT_GT(day.values.at("bars"), 0.0);
        EXPECT_EQ(day.values.at("minute"), s.daily_results[0].values.at("minute"));
    }
    EXPECT_EQ(s.daily_results[1].values.at("bars"), s.daily_results[0].values.at("bars"));
}

TEST_F(MultiDayBacktest, ADayTradeEndsAsItDidWhenEveryDayRanAlone) {
    const std::string mon = write_day(kMon, day_bars(100.0));
    const std::string tue = write_day(kTue, day_bars(102.0));
    const auto both = run({mon, tue}, 20250304, 20250304); // bought and sold on Tuesday alone
    ASSERT_EQ(both.daily_results.size(), 2U);
    EXPECT_TRUE(both.daily_results[0].flat_at_close);
    EXPECT_DOUBLE_EQ(both.daily_results[0].daily_pnl, 0.0);
    EXPECT_DOUBLE_EQ(both.daily_results[1].daily_pnl, run({tue}, 20250304, 20250304).daily_results[0].daily_pnl);
}

TEST_F(MultiDayBacktest, TheDaysRunInDateOrderWhateverTheOrderGiven) {
    const auto s = run({write_day(kTue, day_bars(102.0)), write_day(kMon, day_bars(100.0))},
                       20250303, 20250304);
    ASSERT_EQ(s.daily_results.size(), 2U);
    EXPECT_NE(s.daily_results[0].file_path.find("20250303"), std::string::npos);
    EXPECT_NEAR(final_pnl(s), (2.95 - 1.05) * 100.0, 1e-6);
}

TEST_F(MultiDayBacktest, ASpyCallInTheMoneyAtItsCloseIsDeliveredInSharesHeldOvernight) {
    // Bought on Monday, expiring at Monday's close with SPY at 101: 100 shares delivered at the
    // strike, held into Tuesday, where they are worth the underlying's move.
    const auto s =
        run({write_day(kMon, day_bars(101.0, {kWeekly, kDaily})), write_day(kTue, day_bars(103.0))},
            20250303, 0, 20250303);
    ASSERT_EQ(s.daily_results.size(), 2U);
    EXPECT_TRUE(s.errors.empty()) << s.errors.front();
    EXPECT_FALSE(s.daily_results[0].flat_at_close);
    EXPECT_FALSE(s.daily_results[1].flat_at_close);
    EXPECT_NEAR(s.daily_results[1].daily_pnl, (103.0 - 101.0) * 100.0, 1e-6);
    EXPECT_NEAR(summed(s), final_pnl(s), 1e-6);
}

TEST_F(MultiDayBacktest, ATradingDayMissingWhileSomethingIsHeldStopsTheRun) {
    const std::string mon = write_day(kMon, day_bars(100.0));
    const std::string wed = write_day(kWed, day_bars(101.0));
    const auto held = run({mon, wed}, 20250303, 20250305);
    ASSERT_EQ(held.daily_results.size(), 1U);
    ASSERT_FALSE(held.errors.empty());
    EXPECT_NE(held.errors.back().find("No day between 2025-03-03 and 2025-03-05"),
              std::string::npos);
    // Nothing held at Monday's close: the days do not depend on each other, and both run.
    const auto flat = run({mon, wed}, 20250305, 20250305);
    EXPECT_EQ(flat.daily_results.size(), 2U);
    EXPECT_TRUE(flat.errors.empty());
}

TEST_F(MultiDayBacktest, AContractHeldThatTheNextDayDoesNotQuoteStopsTheRun) {
    const auto s = run({write_day(kMon, day_bars(100.0)), write_day(kTue, day_bars(102.0, {kDaily}))},
                       20250303, 20250304);
    ASSERT_EQ(s.daily_results.size(), 1U);
    ASSERT_FALSE(s.errors.empty());
    EXPECT_NE(s.errors.back().find("does not quote (SPY-20250307-C-100)"),
              std::string::npos)
        << s.errors.back();
}

TEST_F(MultiDayBacktest, WithFlattenMinutesASpyCallIsSoldBeforeItsCloseInsteadOfDelivered) {
    // Bought on Monday, the day it expires, in the money: sold at the 15:50 bar's close order,
    // filled at 16:00, so no shares are delivered.
    const backtest::BacktestTrial trial{
        .strategy_name = "OvernightProbe",
        .strategy_setting =
            {{"buy_day", 20250303}, {"sell_day", 0}, {"expiry", 20250303}, {"strike", 100}},
        .risk_setting = {{"flatten_minutes", 30}},
        .execution = {},
        .fills = true};
    const std::string mon =
        write_day(kMon, day_bars(101.0, {kWeekly, kDaily}, {0, 1, 2, 380, 390}));
    const auto s = backtest::run_backtests({mon}, std::span(&trial, 1), 0.0, "mid",
                                                 utilities::DISABLED)
                       .front();
    EXPECT_TRUE(s.errors.empty()) << s.errors.front();
    ASSERT_EQ(s.daily_results.size(), 1U);
    const auto& day = s.daily_results[0];
    EXPECT_TRUE(day.flat_at_close);
    ASSERT_EQ(day.fills.size(), 2U);
    EXPECT_EQ(day.fills[1].symbol, "SPY-20250303-C-100");
    EXPECT_EQ(day.fills[1].side, model::Side::SELL);
    EXPECT_NEAR(day.daily_pnl, (1.95 - 2.05) * 100.0, 1e-6); // bought at the ask, sold at the bid
}

TEST_F(MultiDayBacktest, TrialsRunSideBySideAndOneThatStopsLeavesTheOthers) {
    const std::vector<std::string> files{write_day(kMon, day_bars(100.0)),
                                         write_day(kTue, day_bars(102.0))};
    backtest::BacktestTrial held{
        .strategy_name = "OvernightProbe",
        .strategy_setting =
            {{"buy_day", 20250303}, {"sell_day", 20250304}, {"expiry", 20250307}, {"strike", 100}},
        .execution = {}};
    backtest::BacktestTrial refused = held;
    refused.risk_setting = {{"no_such_limit", 1}}; // the strategy is added, its limits refused
    const std::vector<backtest::BacktestTrial> trials{held, refused};
    const auto s = backtest::run_backtests(files, trials, 0.0, "mid", utilities::DISABLED,
                                                 /*cache_dir=*/{}, /*threads=*/2);
    ASSERT_EQ(s.size(), 2U);
    EXPECT_TRUE(s[0].errors.empty()) << s[0].errors.front();
    EXPECT_EQ(s[0].daily_results.size(), 2U);
    EXPECT_NEAR(final_pnl(s[0]), (2.95 - 1.05) * 100.0, 1e-6); // as run alone
    EXPECT_TRUE(s[1].daily_results.empty()); // stopped on the first day, skipped on the second
    ASSERT_EQ(s[1].errors.size(), 1U);
    EXPECT_NE(s[1].errors[0].find("Cannot add strategy: Risk limits has no parameter"),
              std::string::npos)
        << s[1].errors[0];
}

TEST_F(MultiDayBacktest, TrialsOnOneThreadShareTheirPortfolioAndGiveTheResultsTheyGiveAlone) {
    const std::vector<std::string> files{write_day(kMon, day_bars(100.0)),
                                         write_day(kTue, day_bars(102.0)),
                                         write_day(kWed, day_bars(99.0))};
    const auto probe = [](double buy_day, double sell_day, double strike) {
        return backtest::BacktestTrial{
            .strategy_name = "OvernightProbe",
            .strategy_setting = {{"buy_day", buy_day},
                                 {"sell_day", sell_day},
                                 {"expiry", 20250307},
                                 {"strike", strike}},
            .execution = {},
            .fills = true};
    };
    backtest::BacktestTrial refused = probe(20250303, 20250305, 100);
    refused.risk_setting = {{"no_such_limit", 1}};
    const std::vector<backtest::BacktestTrial> trials{
        probe(20250303, 20250304, 100), refused, probe(20250304, 20250305, 100),
        probe(20250303, 20250305, 100)};
    const auto together = backtest::run_backtests(files, trials, 0.0, "mid",
                                                        utilities::DISABLED, {}, /*threads=*/1);
    ASSERT_EQ(together.size(), trials.size());
    for (std::size_t t = 0; t < trials.size(); ++t) {
        const auto alone = backtest::run_backtests(files, std::span(&trials[t], 1), 0.0,
                                                         "mid", utilities::DISABLED)
                               .front();
        const backtest::BacktestRunSummary& shared = together[t];
        EXPECT_EQ(shared.errors, alone.errors) << t;
        EXPECT_EQ(final_pnl(shared), final_pnl(alone)) << t;
        ASSERT_EQ(shared.daily_results.size(), alone.daily_results.size()) << t;
        for (std::size_t d = 0; d < alone.daily_results.size(); ++d) {
            const backtest::DailyResult& a = alone.daily_results[d];
            const backtest::DailyResult& b = shared.daily_results[d];
            EXPECT_EQ(b.daily_pnl, a.daily_pnl) << t << " " << d;
            EXPECT_EQ(b.fills.size(), a.fills.size()) << t << " " << d;
            EXPECT_EQ(b.flat_at_close, a.flat_at_close) << t << " " << d;
            ASSERT_EQ(b.metrics.size(), a.metrics.size()) << t << " " << d;
            for (std::size_t m = 0; m < a.metrics.size(); ++m) {
                EXPECT_EQ(b.metrics[m].pnl, a.metrics[m].pnl) << t << " " << d << " " << m;
            }
        }
    }
    EXPECT_NEAR(final_pnl(together[0]), (2.95 - 1.05) * 100.0, 1e-6);
    EXPECT_TRUE(together[1].daily_results.empty());
}

TEST_F(MultiDayBacktest, ADayWithoutDataOrWithoutAStrategyRunsNothing) {
    test::LoneBacktest backtest;
    EXPECT_EQ(backtest.run().errors,
              std::vector<std::string>{"No data loaded. Call use_day() first."});
    backtest.load_backtest_data((dir_ / "SPY-2025-03" / "20250303.parquet").string());
    const std::vector<std::string> unloaded = backtest.run().errors;
    ASSERT_EQ(unloaded.size(), 1U);
    EXPECT_TRUE(unloaded[0].starts_with("No data loaded: ")) << unloaded[0];
    backtest.load_backtest_data(write_day(kMon, day_bars(100.0)));
    EXPECT_EQ(backtest.run().errors,
              std::vector<std::string>{"No strategy added. Call add_strategy() first."});
}

TEST_F(MultiDayBacktest, OrdersStillWorkingAtTheCloseAreCancelled) {
    // A limit far under the market never fills; a market order sent at the last bar never
    // reaches a bar to fill at.
    test::LoneBacktest backtest;
    backtest.configure_execution({});
    backtest.load_backtest_data(write_day(kMon, day_bars(100.0, {kWeekly}, {0, 1, 2, 3})));
    core::Main& main = backtest.main();
    const std::string name =
        main.add_strategy("LateOrderProbe", "SPY", {{"limit_minute", 0}, {"market_minute", 3}});
    const backtest::DailyResult result = backtest.run();
    EXPECT_TRUE(result.errors.empty()) << result.errors.front();
    EXPECT_EQ(result.total_orders, 2);
    EXPECT_TRUE(result.fills.empty());
    EXPECT_FALSE(main.bookkeeping_engine()->has_active_order(name));
    for (const char* order_id : {"backtest_order_1", "backtest_order_2"}) {
        const model::Order* order = main.bookkeeping_engine()->find_order(order_id);
        ASSERT_NE(order, nullptr) << order_id;
        EXPECT_EQ(order->status, model::Status::CANCELLED) << order_id;
    }
    EXPECT_TRUE(main.bookkeeping_engine()->find_holding(name)->is_flat());
}

TEST_F(MultiDayBacktest, AStrategyInErrorStopsTheRunWithItsReason) {
    const backtest::BacktestTrial trial{.strategy_name = "FailingProbe"};
    const auto s = backtest::run_backtests(
                       {write_day(kMon, day_bars(100.0)), write_day(kTue, day_bars(101.0))},
                       std::span(&trial, 1), 0.0, "mid", utilities::DISABLED)
                       .front();
    ASSERT_EQ(s.daily_results.size(), 1U);
    EXPECT_EQ(s.daily_results[0].errors, std::vector<std::string>{"gave up"});
    ASSERT_FALSE(s.errors.empty());
    EXPECT_NE(s.errors.back().find("gave up: the run stops here"),
              std::string::npos)
        << s.errors.back();
}

TEST_F(MultiDayBacktest, ADayThatDoesNotLoadIsSkippedWhenFlatAndStopsTheRunWhenHeld) {
    const std::string mon = write_day(kMon, day_bars(100.0));
    const fs::path broken = dir_ / "SPY-2025-03" / "20250304.parquet";
    std::ofstream(broken) << "not a parquet file";
    const std::string wed = write_day(kWed, day_bars(101.0));

    backtest::BacktestTrial flat{
        .strategy_name = "OvernightProbe",
        .strategy_setting = {{"buy_day", 0}, {"sell_day", 0}, {"expiry", 20250307}},
        .execution = {},
        .minute_metrics = false};
    const auto skipped = backtest::run_backtests({mon, broken.string(), wed},
                                                       std::span(&flat, 1), 0.0, "mid",
                                                       utilities::DISABLED)
                             .front();
    ASSERT_EQ(skipped.daily_results.size(), 3U);
    EXPECT_FALSE(skipped.daily_results[0].load_failed);
    EXPECT_TRUE(skipped.daily_results[1].load_failed);
    EXPECT_FALSE(skipped.daily_results[2].load_failed);
    ASSERT_EQ(skipped.errors.size(), 1U);
    EXPECT_TRUE(skipped.errors[0].starts_with("Cannot load"))
        << skipped.errors[0];
    EXPECT_EQ(skipped.daily_results[0].processed_timesteps +
                  skipped.daily_results[2].processed_timesteps,
              10); // the two days that ran
    for (const backtest::DailyResult& day : skipped.daily_results) {
        EXPECT_TRUE(day.metrics.empty()); // not asked for
    }

    const auto held = run({mon, broken.string(), wed}, 20250303, 20250305);
    ASSERT_EQ(held.daily_results.size(), 1U);
    ASSERT_FALSE(held.errors.empty());
    EXPECT_NE(held.errors.back().find("the holding would skip the day"),
              std::string::npos)
        << held.errors.back();
}

TEST_F(MultiDayBacktest, AnExpiredOptionWithoutAClosingPriceStopsTheRun) {
    // The day's last quotes are half an hour past the close: too late to be its closing price.
    const auto s = run({write_day(kMon, day_bars(101.0, {kWeekly, kDaily}, {0, 1, 2, 3, 420}))},
                       20250303, 0, 20250303);
    ASSERT_EQ(s.daily_results.size(), 1U);
    ASSERT_FALSE(s.errors.empty());
    EXPECT_NE(s.errors.back().find("could not settle"), std::string::npos)
        << s.errors.back();
}

TEST_F(MultiDayBacktest, WithoutTrialsTheDaysAreOnlyCachedAndThenRunTheSameFromTheCache) {
    const std::vector<std::string> files = {write_day(kMon, day_bars(100.0)),
                                            write_day(kTue, day_bars(102.0)),
                                            write_day(kWed, day_bars(101.0))};
    const std::string cache = (dir_ / "cache").string();
    EXPECT_TRUE(backtest::run_backtests(files, {}, 0.0, "mid", utilities::DISABLED, cache)
                    .empty());
    for (const std::string& f : files) {
        EXPECT_TRUE(adapters::day_stored(cache, f)) << f;
    }
    const backtest::BacktestTrial trial{
        .strategy_name = "OvernightProbe",
        .strategy_setting = {{"buy_day", 20250303}, {"sell_day", 20250305}, {"expiry", 20250307},
                             {"strike", 100}},
        .execution = {}};
    for (const std::size_t ahead : {1U, 3U}) {
        const auto cached = backtest::run_backtests(files, std::span(&trial, 1), 0.0, "mid",
                                                          utilities::DISABLED, cache, 1, ahead);
        EXPECT_DOUBLE_EQ(final_pnl(cached.front()), final_pnl(run(files, 20250303, 20250305)));
    }
}

TEST_F(MultiDayBacktest, AStoredDayReadsBackAsItWasReadAndPricedBitForBit) {
    // Prices of whole cents (stored as int32) and prices that are not (stored as doubles).
    std::vector<Bar> cents;
    for (const int minute : {0, 1, 2, 3, 390}) {
        cents.push_back({.minute = minute,
                         .spot = 100.0 + (0.37 * minute),
                         .options = {{kWeekly, 95 * 0.01, 105 * 0.01},
                                     {kDaily, (210 + minute) * 0.01, (230 + minute) * 0.01}}});
    }
    const std::string cache = (dir_ / "cache").string();
    const auto bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
    for (const std::string& day : {write_day(kMon, cents), write_day(kTue, day_bars(102.0))}) {
        const adapters::LoadedDay read = adapters::load_day(day, 0.05, "mid");
        const adapters::LoadedDay stored = adapters::load_day(day, 0.05, "mid", cache);
        read->load_all_chains();
        stored->load_all_chains();
        ASSERT_TRUE(adapters::day_stored(cache, day)) << day;
        const adapters::LoadedDay again = adapters::load_day(day, 0.05, "mid", cache);
        again->load_all_chains();
        for (const adapters::LoadedDay* other : {&stored, &again}) {
            const adapters::HistoricalData& a = *read;
            const adapters::HistoricalData& b = **other;
            ASSERT_EQ(a.loaded_chains(), b.loaded_chains());
            ASSERT_EQ(a.loaded_chains().size(), a.listing()->chains().size());
            ASSERT_EQ(a.minutes(), b.minutes());
            EXPECT_EQ(a.rows(), b.rows());
            EXPECT_EQ(a.day(), b.day());
            EXPECT_EQ(a.portfolio_name(), b.portfolio_name());
            ASSERT_EQ(a.listing()->size(), b.listing()->size());
            for (std::size_t i = 0; i < a.minutes(); ++i) {
                model::PortfolioSnapshot x;
                model::PortfolioSnapshot y;
                a.fill(i, x);
                b.fill(i, y);
                EXPECT_EQ(x.datetime, y.datetime);
                EXPECT_EQ(x.priced, y.priced);
                for (const auto [p, q] : {std::pair{x.underlying_bid, y.underlying_bid},
                                          std::pair{x.underlying_ask, y.underlying_ask},
                                          std::pair{x.greeks_spot, y.greeks_spot}}) {
                    EXPECT_EQ(bits(p), bits(q)) << day << " minute " << i;
                }
                for (std::size_t c = 0; c < a.listing()->size(); ++c) {
                    const model::Greeks g = model::greeks_at(x, c);
                    const model::Greeks h = model::greeks_at(y, c);
                    for (const auto [p, q] :
                         {std::pair{g.delta, h.delta}, std::pair{g.gamma, h.gamma},
                          std::pair{g.theta, h.theta}, std::pair{g.vega, h.vega}}) {
                        EXPECT_EQ(bits(p), bits(q)) << day << " minute " << i;
                    }
                }
                for (const auto column : {&model::Frame::bid, &model::Frame::ask,
                                          &model::Frame::bid_size, &model::Frame::ask_size,
                                          &model::Frame::iv, &model::Frame::forward,
                                          &model::Frame::expiry_years}) {
                    const std::span<const double> u = x.*column;
                    const std::span<const double> v = y.*column;
                    ASSERT_EQ(u.size(), v.size());
                    for (std::size_t c = 0; c < u.size(); ++c) {
                        EXPECT_EQ(bits(u[c]), bits(v[c])) << day << " minute " << i;
                    }
                }
            }
        }
    }
}

TEST_F(MultiDayBacktest, AChainLoadedAlonePricesAsTheWholeDayPricedAtOnceBitForBit) {
    const std::string day = write_day(kMon, day_bars(101.0, {kWeekly, kDaily}));
    const adapters::LoadedDay read = adapters::load_day(day, 0.05, "mid");
    read->load_chains(std::array{std::string("SPY_20250307")});
    read->load_chains(std::array{std::string("SPY_20250303")});
    const adapters::HistoricalData& d = *read;
    ASSERT_EQ(d.loaded_chains().size(), 2U);
    const auto bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
    for (std::size_t i = 0; i < d.minutes(); ++i) {
        model::PortfolioSnapshot x;
        d.fill(i, x);
        model::PortfolioSnapshot whole(d.listing(), d.portfolio_name());
        whole.assign(x);
        // The whole day's listing at once, as it was.
        pricing::price_snapshot(whole, d.rates(), "mid");
        pricing::fit_smiles(whole);
        for (const auto column : {&model::Frame::iv, &model::Frame::forward,
                                  &model::Frame::expiry_years, &model::Frame::expiry_rate,
                                  &model::Frame::smile}) {
            const std::span<const double> u = x.*column;
            const std::span<const double> v = whole.*column;
            ASSERT_EQ(u.size(), v.size());
            for (std::size_t c = 0; c < u.size(); ++c) {
                EXPECT_EQ(bits(u[c]), bits(v[c])) << " minute " << i;
            }
        }
        EXPECT_EQ(bits(x.greeks_spot), bits(whole.greeks_spot)) << " minute " << i;
        for (std::size_t c = 0; c < d.listing()->size(); ++c) {
            EXPECT_EQ(bits(model::greeks_at(x, c).delta), bits(whole.delta[c])) << " minute " << i;
        }
    }
}

TEST_F(MultiDayBacktest, ADayLoadsOnlyTheChainsItsStrategyAsksFor) {
    test::LoneBacktest backtest;
    backtest.load_backtest_data(write_day(kMon, day_bars(101.0, {kWeekly, kDaily})));
    ASSERT_TRUE(backtest.historical_data()->has_data());
    EXPECT_TRUE(backtest.historical_data()->loaded_chains().empty());
    backtest.main().add_strategy("OvernightProbe", "SPY",
                                 {{"buy_day", 20250303}, {"expiry", 20250307}, {"strike", 100}});
    const backtest::DailyResult result = backtest.run();
    EXPECT_TRUE(result.errors.empty()) << result.errors.front();
    EXPECT_EQ(result.fills.size(), 1U); // bought the weekly call it ordered: its chain loaded
    EXPECT_EQ(backtest.historical_data()->loaded_chains(),
              std::vector<std::string>{"SPY_20250307"});
}

TEST_F(MultiDayBacktest, AReplayFillsTheChainsAskedOfItNotThoseLoadedAhead) {
    // Two asks before the first bar, and a chain loaded ahead that nothing asked for.
    const adapters::LoadedDay day = adapters::load_day(
        write_day(kMon, day_bars(101.0, {kWeekly, kDaily, "250314C00100000"})), 0.05, "mid");
    adapters::Replay replay;
    replay.roll(*day);
    day->load_chains(std::array{std::string("SPY_20250314")});
    replay.load(*day, std::array{std::string("SPY_20250307")});
    replay.load(*day, std::array{std::string("SPY_20250303")});
    replay.apply(day, 0);
    const model::Frame& frame = replay.portfolios().find_portfolio("SPY")->frame();
    const model::Listing& listing = *day->listing();
    ASSERT_EQ(listing.chains().size(), 3U);
    for (std::size_t c = 0; c < listing.size(); ++c) {
        const std::string& chain = listing.chains()[listing.chain_of(c)].symbol;
        EXPECT_EQ(frame.bid[c] > 0.0, chain != "SPY_20250314") << chain;
    }
}

/** A trial of one day, made again at `restart_minute` (0: never): its fills. */
auto fills_of(const std::string& file, const std::string& strategy,
              const std::unordered_map<std::string, double>& setting, int restart_minute,
              const model::DailyTable* daily = nullptr) -> std::vector<model::Fill> {
    const backtest::BacktestTrial trial{.strategy_name = strategy,
                                              .strategy_setting = setting,
                                              .execution = {},
                                              .fills = true,
                                              .restart_minute = restart_minute,
                                              .daily = daily};
    auto summary = backtest::run_backtests({file}, std::span(&trial, 1), 0.0, "mid",
                                                 utilities::DISABLED);
    EXPECT_TRUE(summary.front().errors.empty()) << summary.front().errors.front();
    return summary.front().daily_results.front().fills;
}

auto same_fills(const std::vector<model::Fill>& a, const std::vector<model::Fill>& b) -> bool {
    return std::ranges::equal(a, b, [](const model::Fill& x, const model::Fill& y) {
        return x.symbol == y.symbol && x.side == y.side && x.price == y.price &&
               x.quantity == y.quantity && x.datetime == y.datetime;
    });
}

TEST_F(MultiDayBacktest, AStrategyMadeAgainDuringTheDayTradesAsIfItHadRunThrough) {
    // A restart: the strategy's members start over, its holding is the engine's. Holding what it
    // bought, it sells as planned; after selling, it does not buy a second time.
    std::vector<int> minutes(391);
    std::iota(minutes.begin(), minutes.end(), 0);
    std::vector<Bar> bars;
    for (const int minute : minutes) {
        bars.push_back({.minute = minute,
                        .spot = 100.0,
                        .options = {{kDaily, 0.95, 1.05}, {"250303P00100000", 0.95, 1.05}}});
    }
    const std::string day = write_day(kMon, bars);
    const std::unordered_map<std::string, double> random{
        {"seed", 7}, {"earliest_minute", 40}, {"latest_minute", 60}, {"hold_minutes", 30}};
    const std::unordered_map<std::string, double> straddle{
        {"entry_minute", 30}, {"exit_minute", 120}, {"leg1_ratio", 1}, {"leg1_right", 0},
        {"leg2_ratio", 1},    {"leg2_right", 1}};
    for (const auto& [strategy, setting] :
         {std::pair{"RandomBuyStrategy", random}, std::pair{"ComboStrategy", straddle}}) {
        const std::vector<model::Fill> through = fills_of(day, strategy, setting, 0);
        ASSERT_GE(through.size(), 2U) << strategy;
        const int bought = model::kNyse.minutes_since_open(through.front().datetime);
        const int sold = model::kNyse.minutes_since_open(through.back().datetime);
        ASSERT_LT(bought + 2, sold) << strategy;
        EXPECT_TRUE(same_fills(fills_of(day, strategy, setting, bought + 2), through))
            << strategy << ": made again while holding";
        EXPECT_TRUE(same_fills(fills_of(day, strategy, setting, sold + 5), through))
            << strategy << ": made again after selling";
    }
}

TEST_F(MultiDayBacktest, AStrategyMadeAgainKeepsTheDayBeforesRealizedVol) {
    // ComboStrategy enters only within the day before's realized vol, from the values by day: made
    // again before its entry, it still knows it and enters as it would have.
    std::vector<Bar> bars;
    for (int minute = 0; minute <= 390; ++minute) {
        bars.push_back({.minute = minute,
                        .spot = 100.0,
                        .options = {{kDaily, 0.95, 1.05}, {"250303P00100000", 0.95, 1.05}}});
    }
    const std::string day = write_day(kMon, bars);
    model::DailyTable daily;
    daily.set("SPY_minute_rv", {{2025y / February / 28, 0.8}});
    const std::unordered_map<std::string, double> straddle{
        {"entry_minute", 30}, {"exit_minute", 120}, {"leg1_ratio", 1}, {"leg1_right", 0},
        {"leg2_ratio", 1},    {"leg2_right", 1},     {"prev_rv_max", 1}};
    const std::vector<model::Fill> through = fills_of(day, "ComboStrategy", straddle, 0, &daily);
    ASSERT_GE(through.size(), 2U);
    EXPECT_TRUE(same_fills(fills_of(day, "ComboStrategy", straddle, 10, &daily), through));
    EXPECT_TRUE(fills_of(day, "ComboStrategy", straddle, 0).empty()); // not known: no combo
}

TEST(RunBacktests, NoDaysOrNoTrialsRunNothing) {
    const backtest::BacktestTrial trial{.strategy_name = "OvernightProbe"};
    const auto none = backtest::run_backtests({}, std::span(&trial, 1), 0.0, "mid",
                                                    utilities::DISABLED);
    ASSERT_EQ(none.size(), 1U);
    EXPECT_TRUE(none[0].daily_results.empty());
    EXPECT_TRUE(backtest::run_backtests({"20250303.parquet"}, {}, 0.0, "mid",
                                              utilities::DISABLED)
                    .empty());
}

auto contract(const std::string& symbol, model::Product product) -> model::Contract {
    model::Contract c;
    c.symbol = symbol;
    c.product = product;
    c.multiplier = product == model::Product::OPTION ? 100.0 : 1.0;
    return c;
}

auto contracts(std::initializer_list<const char*> options) -> std::vector<model::Contract> {
    std::vector<model::Contract> out{contract("SPY", model::Product::EQUITY)};
    for (const char* o : options) {
        out.push_back(contract(o, model::Product::OPTION));
    }
    return out;
}

TEST(RollContracts, WhatStaysKeepsItsQuotesWhatIsHeldStaysAndAnEmptyChainGoes) {
    model::Portfolios structure;
    structure.ensure_portfolios_created();
    structure.load_contracts(
        contracts({"SPY-20250303-C-100", "SPY-20250307-C-100", "SPY-20250307-P-100"}));
    model::Portfolio* spy = structure.find_portfolio("SPY");
    model::PortfolioSnapshot quotes = spy->snapshot();
    quotes.values(&model::Frame::bid)[*spy->listing().column("SPY-20250307-C-100")] = 1.5;
    quotes.underlying_mark = 100.0;
    quotes.priced = true;
    spy->apply(quotes);

    // Tuesday: Monday's expiry gone, a new Monday's listed, Friday's put held but not listed.
    const auto day = contracts({"SPY-20250307-C-100", "SPY-20250310-C-100"});
    const std::vector<std::string> dropped =
        structure.replace_contracts("SPY", day, {"SPY-20250307-P-100"});

    EXPECT_EQ(dropped, std::vector<std::string>{"SPY_20250303"});
    EXPECT_EQ(structure.find_portfolio("SPY"), spy);
    EXPECT_DOUBLE_EQ(spy->underlying()->mid(), 100.0);
    EXPECT_DOUBLE_EQ(spy->option("SPY-20250307-C-100")->bid(), 1.5);
    EXPECT_TRUE(spy->option("SPY-20250307-P-100")); // held
    EXPECT_FALSE(spy->option("SPY-20250303-C-100"));
    EXPECT_EQ(structure.find_contract("SPY-20250303-C-100"), nullptr);
    std::vector<std::string> order;
    for (const model::Contract& c : spy->listing().contracts()) {
        order.push_back(c.symbol);
    }
    EXPECT_EQ(order, (std::vector<std::string>{"SPY-20250307-C-100", "SPY-20250307-P-100",
                                               "SPY-20250310-C-100"}));
    EXPECT_EQ(spy->chain("SPY_20250307")->strikes().size(), 1U);
}

TEST(TradingCalendar, HolidaysAndWeekendsAreNotTradingDays) {
    EXPECT_TRUE(model::kNyse.is_trading_day(2025y / March / 3));
    EXPECT_FALSE(model::kNyse.is_trading_day(2025y / March / 8));   // a Saturday
    EXPECT_FALSE(model::kNyse.is_trading_day(2025y / January / 9)); // the day of mourning
    EXPECT_FALSE(model::kNyse.is_trading_day(2025y / April / 18));  // Good Friday
    EXPECT_FALSE(model::kNyse.is_trading_day(2026y / July / 3));    // Independence Day, observed
    EXPECT_EQ(model::kNyse.next_trading_day(2025y / March / 7), 2025y / March / 10);
    EXPECT_EQ(model::kNyse.next_trading_day(2025y / April / 17), 2025y / April / 21);
    EXPECT_EQ(model::kNyse.next_trading_day(2025y / December / 31), 2026y / January / 2);
    EXPECT_FALSE(model::kNyse.is_trading_day(2022y / June / 20)); // Juneteenth, its first year
    EXPECT_EQ(model::kNyse.next_trading_day(2021y / December / 23), 2021y / December / 27);
    // The data's first years, 2016 on.
    EXPECT_FALSE(model::kNyse.is_trading_day(2016y / January / 1));
    EXPECT_FALSE(model::kNyse.is_trading_day(2018y / December / 5)); // the day of mourning
    EXPECT_EQ(model::kNyse.next_trading_day(2019y / July / 3), 2019y / July / 5);
}

TEST(TradingCalendar, AnOptionExpiringOnAnEarlyCloseExpiresAt1300) {
    // 2025-07-03: the market closes at 13:00 New York (EDT, UTC-4).
    EXPECT_EQ(model::kNyse.session_close(2025y / July / 3), 13h);
    EXPECT_EQ(model::kNyse.expiry_time(2025y / July / 3), sys_days{2025y / July / 3} + 17h);
    EXPECT_EQ(model::kNyse.expiry_time(2025y / July / 2), sys_days{2025y / July / 2} + 20h);
    EXPECT_EQ(model::kNyse.session_close(2020y / November / 27), 13h);
}

/** A market open around the clock (as crypto's): every day trades, a day is UTC's, from its
 * midnight; its contracts never roll. The same reads as NYSE's, from data. */
TEST(TradingCalendar, AMarketOpenAroundTheClockTradesEveryDayAndNeverRolls) {
    const model::SessionCalendar always{.name = "24H", .zone = "UTC", .weekends_closed = false};
    EXPECT_TRUE(always.is_trading_day(2025y / March / 8)); // a Saturday
    EXPECT_EQ(always.next_trading_day(2025y / March / 8), 2025y / March / 9);
    const model::DateTime late = sys_days{2025y / March / 8} + 23h + 30min;
    EXPECT_EQ(always.date_of(late), 2025y / March / 8);
    EXPECT_EQ(always.minutes_since_open(late), 23 * 60 + 30);
    EXPECT_EQ(always.expiry_time(2025y / March / 8), sys_days{2025y / March / 9});
    EXPECT_FALSE(always.roll);
    // In New York it is 19:30 on the 8th, a day NYSE is closed.
    EXPECT_EQ(model::kNyse.date_of(late), 2025y / March / 8);
    EXPECT_FALSE(model::kNyse.is_trading_day(model::kNyse.date_of(late)));
}

} // namespace
