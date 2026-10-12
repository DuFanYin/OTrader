/**
 * The live event loop (EventEngine started) over the engine itself: the backtest assembly with
 * its events through the event thread, so the data's clock tells which snapshot was applied.
 * Order updates, fills and tasks are never dropped and keep their order, the event thread
 * putting some itself; a portfolio's snapshots keep only the newest; the event thread runs the
 * timer itself; and a day replayed through the event thread trades as the backtest does.
 */
#include "core/engines/execution.hpp"
#include "core/engines/option_strategy.hpp"
#include "model/event.hpp"
#include "core/engine_backtest.hpp"
#include "core/engine_event.hpp"
#include "strategy/template.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using model::Event;
using model::EventType;
using namespace std::chrono_literals;

/** The backtest assembly, its events through the event thread: started, with no timer of its
 * own (a Timer only as put). */
class Threaded final : public core::backtest::MainEngine {
  public:
    Threaded() {
        event_engine_ = std::make_unique<core::EventEngine>(this, 0ms);
        event_engine_->start();
    }
    ~Threaded() { event_engine_->stop(); } // its thread is done before the engine goes
    Threaded(const Threaded&) = delete;
    Threaded& operator=(const Threaded&) = delete;
};

/** Holds the event thread in a task until released: what is put meanwhile waits for it. */
class Hold {
  public:
    explicit Hold(core::Engine& engine)
        : thread_([&engine, this] {
              engine.on_event_thread([this] {
                  running_.count_down();
                  released_.wait(false);
              });
          }) {
        running_.wait();
    }
    ~Hold() { release(); }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;

    void release() {
        released_ = true;
        released_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

  private:
    std::latch running_{1};
    std::atomic<bool> released_{false};
    std::thread thread_;
};

void put_trade(core::Engine& engine, const std::string& tradeid) {
    model::TradeData* t = engine.acquire_trade();
    ASSERT_NE(t, nullptr);
    t->tradeid = tradeid;
    t->orderid = "O";
    t->symbol = "SPY";
    t->volume = 1;
    engine.put_event(Event(EventType::Trade, t));
}

auto at(int seconds) -> model::DateTime {
    return model::DateTime{} + std::chrono::hours(24 * 365 * 55) + std::chrono::seconds(seconds);
}

TEST(LiveEventLoop, AFloodOfFillsIsBookedWhole) {
    Threaded engine;
    constexpr int kProducers = 4;
    constexpr int kEach = 5000; // far past the old queue's 512
    {
        Hold hold(engine); // nothing is processed while they pour in
        std::vector<std::jthread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&engine, p] {
                for (int i = 0; i < kEach; ++i) {
                    put_trade(engine, std::format("T{}-{}", p, i));
                }
            });
        }
    }
    const auto booked =
        engine.on_event_thread([&] { return engine.execution_engine()->get_all_trades().size(); });
    EXPECT_EQ(booked, static_cast<std::size_t>(kProducers * kEach));
}

TEST(LiveEventLoop, TheEventThreadPutsWithoutWaitingOnItself) {
    Threaded engine; // as IbGateway reports an order SUBMITTING from inside send_order
    engine.on_event_thread([&] {
        for (int i = 0; i < 20'000; ++i) {
            put_trade(engine, std::format("T{}", i));
        }
    });
    const auto booked =
        engine.on_event_thread([&] { return engine.execution_engine()->get_all_trades().size(); });
    EXPECT_EQ(booked, 20'000U);
}

TEST(LiveEventLoop, OnlyThePortfoliosNewestSnapshotIsApplied) {
    Threaded engine;
    {
        Hold hold(engine);
        model::PortfolioSnapshot s;
        s.portfolio_name = "backtest";
        for (int i = 1; i <= 1000; ++i) {
            s.datetime = at(i);
            engine.put_snapshot(s);
        }
    }
    EXPECT_EQ(engine.on_event_thread([&] { return engine.now(); }), at(1000));
}

TEST(LiveEventLoop, ATaskComesAfterWhatWasPutBeforeIt) {
    Threaded engine;
    model::PortfolioSnapshot s;
    s.portfolio_name = "backtest";
    for (int i = 0; i < 200; ++i) {
        const std::string id = std::format("T{}", i);
        put_trade(engine, id);
        s.datetime = at(i);
        engine.put_snapshot(s);
        const auto [booked, now] = engine.on_event_thread([&] {
            return std::pair(engine.execution_engine()->get_trade(id) != nullptr, engine.now());
        });
        EXPECT_TRUE(booked) << id;
        EXPECT_EQ(now, at(i));
    }
}

TEST(LiveEventLoop, TheEventThreadRunsTheTimerOnSchedule) {
    core::EventEngine events(nullptr, 20ms);
    events.start();
    std::this_thread::sleep_for(510ms);
    events.stop();
    EXPECT_GE(events.timer_ticks(), 20U); // 25 on schedule; a loaded machine may skip a few
    EXPECT_LE(events.timer_ticks(), 26U);
}

// ---- A day through the event thread, against the backtest

/** What a day's run leaves: the fills in id order, the strategy's P&L, the orders sent. */
struct DayResult {
    std::vector<std::tuple<std::string, std::string, int, double, double,
                           model::DateTime>>
        fills;
    double pnl = 0.0;
    std::size_t orders = 0;
};

auto result_of(core::Engine& engine) -> DayResult {
    DayResult r;
    for (const model::TradeData& t : engine.execution_engine()->get_all_trades()) {
        r.fills.emplace_back(t.tradeid, t.symbol,
                             static_cast<int>(t.direction), t.price, t.volume,
                             t.datetime);
    }
    std::ranges::sort(r.fills);
    if (const model::StrategyHolding* h = engine.option_strategy_engine()->get_strategy_holding()) {
        r.pnl = h->summary.pnl;
    }
    r.orders = engine.execution_engine()->get_all_orders().size();
    return r;
}

auto test_day() -> std::string {
    if (const char* day = std::getenv("OPTRA_TEST_DAY"); day != nullptr && *day != '\0') {
        return day;
    }
    return std::string(OPTRA_DATA_DIR) + "/SPY/SPY-2025-03/20250303.parquet";
}

/** As MainEngine::run, each bar through the event thread: the snapshot, then the orders of the
 * bar before filled at its quotes, then the timer. */
auto replay_through_the_event_thread(Threaded& engine) -> DayResult {
    adapters::BacktestDataEngine* data = engine.get_data_engine();
    adapters::BacktestGateway* venue = engine.backtest_gateway();
    engine.on_event_thread([&] {
        strategy::OptionStrategyTemplate* s = engine.option_strategy_engine()->get_strategy();
        s->on_init();
        s->on_start();
    });
    int step = 0;
    data->iter_timesteps([&](adapters::Timestamp, const adapters::TimestepFrameColumnar&) {
        engine.put_snapshot(data->get_precomputed_snapshot(step++));
        engine.on_event_thread([&] { venue->execute_pending_orders(); }); // its fills queue up
        engine.put_event(Event(EventType::Timer));                         // after them
        engine.on_event_thread([] {});                                     // the bar is done
        return true;
    });
    return engine.on_event_thread([&] { return result_of(engine); });
}

TEST(LiveEventLoop, ADayTradesAsInTheBacktest) {
    const std::string day = test_day();
    if (!std::filesystem::exists(day)) {
        GTEST_SKIP() << "no day to replay: " << day << " (set OPTRA_TEST_DAY)";
    }
    std::size_t fills = 0;
    for (const char* strategy : {"StraddleInventoryScalperStrategy", "IvMeanRevertStrategy"}) {
        core::backtest::MainEngine backtest;
        backtest.load_backtest_data(day);
        ASSERT_TRUE(backtest.get_data_engine()->has_data()) << day;
        backtest.add_strategy(strategy, backtest.get_data_engine()->portfolio_name(), {});
        backtest.run();
        const DayResult expected = result_of(backtest);

        Threaded live;
        live.load_backtest_data(day);
        live.add_strategy(strategy, live.get_data_engine()->portfolio_name(), {});
        const DayResult got = replay_through_the_event_thread(live);

        EXPECT_EQ(got.fills, expected.fills) << strategy;
        EXPECT_EQ(got.pnl, expected.pnl) << strategy;
        EXPECT_EQ(got.orders, expected.orders) << strategy;
        fills += expected.fills.size();
    }
    EXPECT_GT(fills, 0U); // the day traded: the comparison says something
}

TEST(ZeroDteBuyers, EachBuysOneOptionExpiringThatDayAndSellsItBeforeTheClose) {
    const std::string day = test_day();
    if (!std::filesystem::exists(day)) {
        GTEST_SKIP() << "no day to replay: " << day << " (set OPTRA_TEST_DAY)";
    }
    for (const char* strategy :
         {"LateDayMomentumStrategy", "RandomBuyStrategy", "OpeningRangeBreakoutStrategy"}) {
        core::backtest::MainEngine backtest;
        backtest.load_backtest_data(day);
        ASSERT_TRUE(backtest.get_data_engine()->has_data()) << day;
        backtest.add_strategy(strategy, backtest.get_data_engine()->portfolio_name(), {});
        const core::backtest::BacktestResult result = backtest.run();
        EXPECT_TRUE(result.errors.empty()) << strategy;
        const std::vector<model::TradeData>& fills = result.trades;
        if (std::string_view(strategy) == "OpeningRangeBreakoutStrategy" && fills.empty()) {
            continue; // a day that never left its opening range
        }
        ASSERT_EQ(fills.size(), 2U) << strategy;
        EXPECT_EQ(fills[0].direction, model::Direction::LONG) << strategy;
        EXPECT_EQ(fills[1].direction, model::Direction::SHORT) << strategy;
        EXPECT_EQ(fills[0].symbol, fills[1].symbol) << strategy;
        EXPECT_EQ(fills[0].volume, fills[1].volume) << strategy;
        const std::optional<model::OptionId> id = model::parse_option_symbol(fills[0].symbol);
        ASSERT_TRUE(id) << strategy;
        ASSERT_NE(fills[1].datetime, model::DateTime{}) << strategy;
        EXPECT_EQ(id->expiry, model::new_york_date(fills[1].datetime)) << strategy;
        EXPECT_LT(fills[1].datetime, model::option_expiry_time(id->expiry)) << strategy;
        EXPECT_TRUE(backtest.option_strategy_engine()->get_strategy_holding()->is_flat())
            << strategy;
    }
}

} // namespace
