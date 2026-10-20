/**
 * The live event loop (EventLoop started) over the engine itself: the backtest assembly with
 * its events through the event thread, a portfolio's time telling which snapshot was applied.
 * Order updates, fills and tasks are never dropped and keep their order, the event thread
 * putting some itself; a portfolio's snapshots keep only the newest; the event thread runs the
 * timer itself; and a day replayed through the event thread trades as the backtest does.
 */
#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "core/strategy/strategy_engine.hpp"
#include "model/session_calendar.hpp"
#include "model/event.hpp"
#include "tests/lone_backtest.hpp"
#include "core/event_loop.hpp"
#include "strategy/strategy.hpp"
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
class Threaded final : public test::LoneBacktest {
  public:
    Threaded() {
        main().event_loop()->set_interval(0ms);
        main().event_loop()->start();
    }
};

/** Holds the event thread in a task until released: what is put meanwhile waits for it. */
class Hold {
  public:
    explicit Hold(core::Main& engine)
        : thread_([&engine, this] {
              engine.event_loop()->on_event_thread([this] {
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

void put_fill(core::Main& engine, const std::string& fill_id) {
    model::Fill* t = engine.event_loop()->acquire_fill();
    ASSERT_NE(t, nullptr);
    t->fill_id = fill_id;
    t->order_id = "O";
    t->symbol = "SPY";
    t->quantity = 1;
    engine.event_loop()->put_event(Event(EventType::Fill, t));
}

auto at(int seconds) -> model::DateTime {
    return model::DateTime{} + std::chrono::hours(24 * 365 * 55) + std::chrono::seconds(seconds);
}

/** A snapshot of "P", a portfolio of no options made for it, whose time says which of its
 * snapshots was applied last. */
auto snapshot_of_p(core::Main& engine) -> model::PortfolioSnapshot {
    return engine.event_loop()->on_event_thread([&] {
        engine.portfolios()->ensure_portfolio("P");
        model::PortfolioSnapshot s = engine.portfolios()->find_portfolio("P")->snapshot();
        s.priced = true;
        return s;
    });
}

auto applied_at(core::Main& engine) -> model::DateTime {
    return engine.event_loop()->on_event_thread(
        [&] { return engine.find_portfolio("P")->datetime(); });
}

TEST(LiveEventLoop, BeforeStartAnOrderAndAFillAreBookedInPlace) {
    test::LoneBacktest backtest; // its loop never starts
    core::Main& engine = backtest.main();
    const std::string call = "SPXW-20250804-C-5000";
    model::Order order;
    order.order_id = "OID-1";
    order.symbol = call;
    order.legs = {{.ratio = 1, .symbol = call}};
    order.quantity = 2;
    engine.bookkeeping_engine()->add_order("s1", order);
    order.status = model::Status::WORKING;
    engine.event_loop()->put_order(order);
    ASSERT_NE(engine.bookkeeping_engine()->find_order("OID-1"), nullptr);
    EXPECT_TRUE(engine.bookkeeping_engine()->is_active("OID-1"));

    model::Fill fill;
    fill.order_id = "OID-1";
    fill.fill_id = "TID-1";
    fill.symbol = call;
    fill.side = model::Side::BUY;
    fill.price = 1.25;
    fill.quantity = 2;
    engine.event_loop()->put_fill(fill);
    EXPECT_NE(engine.bookkeeping_engine()->find_fill("TID-1"), nullptr);
    const model::StrategyHolding* holding = engine.bookkeeping_engine()->find_holding("s1");
    ASSERT_NE(holding, nullptr);
    ASSERT_TRUE(holding->options.contains(call));
    EXPECT_EQ(holding->options.at(call).quantity, 2);
}

TEST(LiveEventLoop, AFloodOfFillsIsBookedWhole) {
    Threaded threaded;
    core::Main& engine = threaded.main();
    constexpr int kProducers = 4;
    constexpr int kEach = 5000; // far past the old queue's 512
    {
        Hold hold(engine); // nothing is processed while they pour in
        std::vector<std::jthread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&engine, p] {
                for (int i = 0; i < kEach; ++i) {
                    put_fill(engine, std::format("T{}-{}", p, i));
                }
            });
        }
    }
    const auto booked =
        engine.event_loop()->on_event_thread(
            [&] { return engine.bookkeeping_engine()->fills().size(); });
    EXPECT_EQ(booked, static_cast<std::size_t>(kProducers * kEach));
}

TEST(LiveEventLoop, TheEventThreadPutsWithoutWaitingOnItself) {
    Threaded threaded; // as IbGateway reports an order SUBMITTING from inside send_order
    core::Main& engine = threaded.main();
    engine.event_loop()->on_event_thread([&] {
        for (int i = 0; i < 20'000; ++i) {
            put_fill(engine, std::format("T{}", i));
        }
    });
    const auto booked =
        engine.event_loop()->on_event_thread(
            [&] { return engine.bookkeeping_engine()->fills().size(); });
    EXPECT_EQ(booked, 20'000U);
}

TEST(LiveEventLoop, OnlyThePortfoliosNewestSnapshotIsApplied) {
    Threaded threaded;
    core::Main& engine = threaded.main();
    model::PortfolioSnapshot s = snapshot_of_p(engine);
    {
        Hold hold(engine);
        for (int i = 1; i <= 1000; ++i) {
            s.datetime = at(i);
            engine.event_loop()->put_snapshot(s);
        }
    }
    EXPECT_EQ(applied_at(engine), at(1000));
}

TEST(LiveEventLoop, ATaskComesAfterWhatWasPutBeforeIt) {
    Threaded threaded;
    core::Main& engine = threaded.main();
    model::PortfolioSnapshot s = snapshot_of_p(engine);
    for (int i = 0; i < 200; ++i) {
        const std::string id = std::format("T{}", i);
        put_fill(engine, id);
        s.datetime = at(i);
        engine.event_loop()->put_snapshot(s);
        const auto [booked, applied] = engine.event_loop()->on_event_thread([&] {
            return std::pair(engine.bookkeeping_engine()->find_fill(id) != nullptr,
                             engine.find_portfolio("P")->datetime());
        });
        EXPECT_TRUE(booked) << id;
        EXPECT_EQ(applied, at(i));
    }
}

TEST(LiveEventLoop, TheEventThreadRunsTheTimerOnSchedule) {
    core::EventLoop events(nullptr, {}, {}, 20ms);
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

auto result_of(core::Main& engine) -> DayResult {
    DayResult r;
    for (const model::Fill& t : engine.bookkeeping_engine()->fills()) {
        r.fills.emplace_back(t.fill_id, t.symbol,
                             static_cast<int>(t.side), t.price, t.quantity,
                             t.datetime);
    }
    std::ranges::sort(r.fills);
    const std::string& name = engine.strategy_engine()->strategies().front().name;
    if (const model::StrategyHolding* h = engine.bookkeeping_engine()->find_holding(name)) {
        r.pnl = h->summary.pnl;
    }
    r.orders = engine.bookkeeping_engine()->order_count();
    return r;
}

auto test_day() -> std::string {
    if (const char* day = std::getenv("OPTRA_TEST_DAY"); day != nullptr && *day != '\0') {
        return day;
    }
    return std::string(OPTRA_DATA_DIR) + "/synthetic/SPY/SPY-2025-03/20250303.parquet";
}

/** As Backtest::run, each bar through the event thread: the engine's time, the snapshot, then the
 * orders of the bar before filled at its quotes, then the timer. */
auto replay_through_the_event_thread(Threaded& live) -> DayResult {
    adapters::HistoricalData* data = live.historical_data();
    adapters::BacktestGateway* venue = live.backtest_gateway();
    core::Main& engine = live.main();
    core::EventLoop& events = *engine.event_loop();
    const std::string& name = live.strategy()->name;
    engine.init_strategy(name);
    engine.start_strategy(name);
    data->load_all_chains(); // as a source streaming the whole day; the backtest loads what it asks
    model::PortfolioSnapshot snapshot(data->listing(), data->portfolio_name());
    for (std::size_t bar = 0; bar < data->minutes(); ++bar) {
        data->fill(bar, snapshot);
        events.on_event_thread([&] { engine.set_time(snapshot.datetime); }); // as run_bar
        events.put_snapshot(snapshot);
        events.on_event_thread([&] { venue->execute_pending_orders(); }); // its fills queue up
        events.put_event(Event(EventType::Timer));                         // after them
        events.on_event_thread([] {});                                     // the bar is done
    }
    return events.on_event_thread([&] { return result_of(engine); });
}

TEST(LiveEventLoop, ADayTradesAsInTheBacktest) {
    const std::string day = test_day();
    if (!std::filesystem::exists(day)) {
        GTEST_SKIP() << "no day to replay: " << day << " (set OPTRA_TEST_DAY)";
    }
    std::size_t fills = 0;
    for (const char* strategy : {"ComboStrategy", "RandomBuyStrategy"}) {
        test::LoneBacktest backtest;
        backtest.load_backtest_data(day);
        ASSERT_TRUE(backtest.historical_data()->has_data()) << day;
        backtest.main().add_strategy(strategy, backtest.historical_data()->portfolio_name(), {});
        backtest.run();
        const DayResult expected = result_of(backtest.main());

        Threaded live;
        live.load_backtest_data(day);
        live.main().add_strategy(strategy, live.historical_data()->portfolio_name(), {});
        const DayResult got = replay_through_the_event_thread(live);

        EXPECT_EQ(got.fills, expected.fills) << strategy;
        EXPECT_EQ(got.pnl, expected.pnl) << strategy;
        EXPECT_EQ(got.orders, expected.orders) << strategy;
        fills += expected.fills.size();
    }
    EXPECT_GT(fills, 0U); // the day traded: the comparison says something
}

TEST(ZeroDteBuyers, TheControlBuysOneOptionExpiringThatDayAndSellsItBeforeTheClose) {
    const std::string day = test_day();
    if (!std::filesystem::exists(day)) {
        GTEST_SKIP() << "no day to replay: " << day << " (set OPTRA_TEST_DAY)";
    }
    for (const char* strategy : {"RandomBuyStrategy"}) {
        test::LoneBacktest backtest;
        backtest.load_backtest_data(day);
        ASSERT_TRUE(backtest.historical_data()->has_data()) << day;
        backtest.main().add_strategy(strategy, backtest.historical_data()->portfolio_name(), {});
        const backtest::DailyResult result = backtest.run();
        EXPECT_TRUE(result.errors.empty()) << strategy;
        const std::vector<model::Fill>& fills = result.fills;
        ASSERT_EQ(fills.size(), 2U) << strategy;
        EXPECT_EQ(fills[0].side, model::Side::BUY) << strategy;
        EXPECT_EQ(fills[1].side, model::Side::SELL) << strategy;
        EXPECT_EQ(fills[0].symbol, fills[1].symbol) << strategy;
        EXPECT_EQ(fills[0].quantity, fills[1].quantity) << strategy;
        const std::optional<model::OptionId> id = model::parse_option_symbol(fills[0].symbol);
        ASSERT_TRUE(id) << strategy;
        ASSERT_NE(fills[1].datetime, model::DateTime{}) << strategy;
        EXPECT_EQ(id->expiry, model::kNyse.date_of(fills[1].datetime)) << strategy;
        EXPECT_LT(fills[1].datetime, model::kNyse.expiry_time(id->expiry)) << strategy;
        const std::string& name = backtest.strategy()->name;
        EXPECT_TRUE(backtest.main().bookkeeping_engine()->find_holding(name)->is_flat())
            << strategy;
    }
}

} // namespace
