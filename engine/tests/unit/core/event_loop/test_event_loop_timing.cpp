/** The event loop alone: snapshot slots, order against tasks, drops, start and stop, the timer. */
#include "core/event_loop.hpp"
#include "model/event.hpp"
#include "utilities/latency.hpp"
#include "utilities/logger.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct Recorder final : utilities::Logger {
    void log(const utilities::LogRecord& record) override {
        const std::scoped_lock lock(mutex);
        lines.push_back(record.msg);
    }
    auto count(std::string_view part) -> std::size_t {
        const std::scoped_lock lock(mutex);
        return static_cast<std::size_t>(std::ranges::count_if(
            lines, [&](const std::string& l) { return l.find(part) != std::string::npos; }));
    }
    std::mutex mutex;
    std::vector<std::string> lines;
};

/** What reached the handler, in order (event thread; read after a barrier). */
struct Seen {
    std::vector<std::string> snapshots; // "portfolio@seconds"
    std::vector<std::chrono::steady_clock::time_point> timers;
};

auto snapshot(const std::string& portfolio, int seconds) -> model::PortfolioSnapshot {
    model::PortfolioSnapshot s(nullptr, portfolio);
    s.datetime = model::DateTime{} + std::chrono::seconds(seconds);
    return s;
}

auto handler(Seen& seen) -> core::EventLoop::Handler {
    return [&seen](const model::Event& e) {
        if (const auto* p = std::get_if<model::PortfolioSnapshot*>(&e.data); p && *p) {
            seen.snapshots.push_back(std::format(
                "{}@{}", (*p)->portfolio_name,
                std::chrono::duration_cast<std::chrono::seconds>((*p)->datetime.time_since_epoch())
                    .count()));
        } else if (e.type == model::EventType::Timer) {
            seen.timers.push_back(std::chrono::steady_clock::now());
        }
    };
}

/** Holds the event thread inside a task until released: what is put meanwhile waits. */
struct Hold {
    explicit Hold(core::EventLoop& loop) {
        auto in = entered.get_future();
        waiter = std::thread([&loop, this] {
            loop.on_event_thread([this] {
                entered.set_value();
                release.get_future().wait();
            });
        });
        in.wait();
    }
    ~Hold() {
        release.set_value();
        waiter.join();
    }
    std::promise<void> entered;
    std::promise<void> release;
    std::thread waiter;
};

TEST(EventLoopTiming, BeforeStartASnapshotReachesTheHandlerInPlace) {
    Seen seen;
    core::EventLoop loop(nullptr, handler(seen), {}, 0ms);
    loop.put_snapshot(snapshot("SPY", 1));
    EXPECT_EQ(seen.snapshots, (std::vector<std::string>{"SPY@1"}));
}

TEST(EventLoopTiming, EachPortfolioKeepsOnlyItsNewestUnappliedSnapshot) {
    Seen seen;
    core::EventLoop loop(nullptr, handler(seen), {}, 0ms);
    loop.start();
    {
        const Hold hold(loop);
        loop.put_snapshot(snapshot("SPY", 1));
        loop.put_snapshot(snapshot("SPXW", 1));
        loop.put_snapshot(snapshot("SPY", 2));
        loop.put_snapshot(snapshot("SPY", 3));
        loop.put_snapshot(snapshot("SPXW", 2));
    }
    loop.on_event_thread([] {});
    // The slots in the order the portfolios first came, each with its latest.
    EXPECT_EQ(seen.snapshots, (std::vector<std::string>{"SPY@3", "SPXW@2"}));
    loop.stop();
}

TEST(EventLoopTiming, ATaskSeesEverySnapshotPutBeforeIt) {
    Seen seen;
    core::EventLoop loop(nullptr, handler(seen), {}, 0ms);
    loop.start();
    for (int i = 1; i <= 50; ++i) {
        loop.put_snapshot(snapshot("SPY", i));
        const std::string last = loop.on_event_thread([&] { return seen.snapshots.back(); });
        EXPECT_EQ(last, std::format("SPY@{}", i));
    }
    loop.stop();
}

TEST(EventLoopTiming, ASnapshotOfOnePortfolioTooManyIsDroppedAndTold) {
    Recorder log;
    Seen seen;
    core::EventLoop loop(&log, handler(seen), {}, 0ms);
    loop.start();
    {
        const Hold hold(loop);
        for (std::size_t i = 0; i <= core::EventLoop::kMaxPortfolios; ++i) {
            loop.put_snapshot(snapshot(std::format("P{}", i), 1));
        }
    }
    loop.on_event_thread([] {});
    EXPECT_EQ(seen.snapshots.size(), core::EventLoop::kMaxPortfolios);
    EXPECT_EQ(log.count("Dropped a Snapshot event: more than 64 portfolios (1 dropped so far)"),
              1U);
    loop.stop();
}

TEST(EventLoopTiming, ADropReleasesItsPayloadAndOnlySomeSnapshotDropsAreLogged) {
    Recorder log;
    core::EventLoop loop(&log, {}, {}, 0ms);
    model::Order* order = loop.acquire_order();
    loop.drop(model::Event(model::EventType::Order, order), "test");
    EXPECT_EQ(loop.acquire_order(), order); // back in its pool
    EXPECT_EQ(log.count("Dropped an Order event: test"), 1U);

    for (int i = 0; i < 1000; ++i) {
        loop.drop(model::Event(model::EventType::Snapshot, loop.acquire_snapshot()), "full");
    }
    // Superseded anyway: the first drop and every thousandth are told (here the 1000th overall).
    EXPECT_EQ(log.count("Dropped a Snapshot event"), 1U);
    loop.drop(model::Event(model::EventType::Snapshot, loop.acquire_snapshot()), "full");
    EXPECT_EQ(log.count("Dropped a Snapshot event"), 1U);
    for (int i = 0; i < 998; ++i) {
        loop.drop(model::Event(model::EventType::Snapshot), "full");
    }
    EXPECT_EQ(log.count("Dropped a Snapshot event"), 2U); // the 2000th drop
}

TEST(EventLoopTiming, StartingTwiceStartsOneThreadAndStopWaitsForWhatWasQueued) {
    std::atomic<int> handled{0};
    core::EventLoop loop(nullptr, [&](const model::Event&) { ++handled; }, {}, 0ms);
    loop.start();
    loop.start();
    std::thread::id first;
    loop.on_event_thread([&] { first = std::this_thread::get_id(); });
    {
        const Hold hold(loop);
        for (int i = 0; i < 500; ++i) {
            loop.put_order(model::Order{.order_id = std::to_string(i)});
        }
    }
    loop.stop();
    EXPECT_EQ(handled.load(), 500);
    EXPECT_NE(first, std::this_thread::get_id());
    loop.stop(); // a second stop returns
}

TEST(EventLoopTiming, SpinningOnAPinnedCoreItRunsAndStopsAsSleeping) {
    Recorder log;
    std::atomic<int> handled{0};
    core::EventLoop loop(&log, [&](const model::Event&) { ++handled; }, {}, 0ms);
    loop.set_spin(200us);
    loop.set_cpu(0);
    loop.start();
    for (int i = 0; i < 2000; ++i) {
        loop.put_fill(model::Fill{.fill_id = std::to_string(i)});
        if (i % 100 == 0) {
            EXPECT_EQ(loop.on_event_thread([&] { return handled.load(); }), i + 1);
        }
    }
    loop.stop();
    EXPECT_EQ(handled.load(), 2000);
    EXPECT_EQ(log.count("could not be pinned"), 0U);
}

TEST(EventLoopTiming, TheEnvironmentSetsSpinAndCoreAndABadCoreIsTold) {
    Recorder log;
    core::EventLoop loop(&log, {}, {}, 0ms);
    ::setenv("OPTRA_EVENT_SPIN_US", "50", 1);
    ::setenv("OPTRA_EVENT_CPU", "1023", 1); // a core this machine does not have
    loop.configure_from_environment();
    ::unsetenv("OPTRA_EVENT_SPIN_US");
    ::unsetenv("OPTRA_EVENT_CPU");
    loop.start();
    EXPECT_EQ(loop.on_event_thread([] { return 7; }), 7); // runs unpinned
    loop.stop();
    EXPECT_EQ(log.count("The event thread could not be pinned to core 1023"), 1U);
}

TEST(EventLoopTiming, AfterATimerThatOverrunsTheNextComesAnIntervalLaterNotInABurst) {
    Seen seen;
    bool stalled = false;
    core::EventLoop loop(
        nullptr,
        [&](const model::Event& e) {
            handler(seen)(e);
            if (e.type == model::EventType::Timer && !stalled) {
                stalled = true;
                std::this_thread::sleep_for(30ms); // ten intervals behind
            }
        },
        {}, 3ms);
    loop.start();
    while (loop.on_event_thread([&] { return seen.timers.size(); }) < 6) {
        std::this_thread::sleep_for(1ms);
    }
    loop.stop();
    ASSERT_GE(seen.timers.size(), 6U);
    EXPECT_GE(seen.timers[1] - seen.timers[0], 30ms);
    // Back on schedule from the stall: not the ten missed ticks at once.
    EXPECT_GE(seen.timers[3] - seen.timers[1], 4ms);
    EXPECT_GE(loop.timer_ticks(), 6U);
}

TEST(EventLoopTiming, QuoteToAppliedAndQuoteToOrderAreMeasuredAndReportedAtStop) {
    Recorder log;
    core::EventLoop* self = nullptr;
    core::EventLoop loop(
        &log,
        [&](const model::Event& e) {
            if (e.type == model::EventType::Snapshot) {
                self->note_order_sent(); // an order sent while the quote is applied
            }
        },
        [&] { return "REPORT " + self->latency_report(); }, 0ms);
    self = &loop;
    loop.start();
    model::PortfolioSnapshot s = snapshot("SPY", 1);
    s.quoted_tsc = utilities::tsc_now();
    loop.put_snapshot(s);
    loop.on_event_thread([] {});
    loop.note_order_sent(); // not while a quote is applied: not counted
    const std::string report = loop.latency_report();
    EXPECT_NE(report.find("quote -> applied: 1 samples"), std::string::npos) << report;
    EXPECT_NE(report.find("quote -> order: 1 samples"), std::string::npos) << report;
    loop.stop();
    EXPECT_EQ(log.count("REPORT Latency (event thread, since start):"), 1U);
}

} // namespace
