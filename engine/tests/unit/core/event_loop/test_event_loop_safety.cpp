#include "core/event_loop.hpp"
#include "model/event.hpp"
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

TEST(EventLoopSafety, PutBeforeStartDoesNotCrash) {
    core::EventLoop loop;

    // Not started => should reject safely (no crash).
    auto* snap = loop.acquire_snapshot();
    ASSERT_NE(snap, nullptr);
    snap->portfolio_name = "SPXW";
    loop.put_event(model::Event(model::EventType::Snapshot, snap));

    auto* ord = loop.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->order_id = "OID";
    loop.put_event(model::Event(model::EventType::Order, ord));

    auto* tr = loop.acquire_fill();
    ASSERT_NE(tr, nullptr);
    tr->fill_id = "TID";
    loop.put_event(model::Event(model::EventType::Fill, tr));
}

TEST(EventLoopSafety, StopReturns) {
    core::EventLoop loop;
    loop.start();

    // Enqueue a burst of payload events; even if some get dropped, payloads must be released.
    for (int i = 0; i < 200; ++i) {
        auto* ord = loop.acquire_order();
        ASSERT_NE(ord, nullptr);
        ord->order_id = "OID-" + std::to_string(i);
        loop.put_event(model::Event(model::EventType::Order, ord));
    }
    for (int i = 0; i < 200; ++i) {
        auto* tr = loop.acquire_fill();
        ASSERT_NE(tr, nullptr);
        tr->fill_id = "TID-" + std::to_string(i);
        loop.put_event(model::Event(model::EventType::Fill, tr));
    }

    loop.stop();
}

TEST(EventLoopSafety, BeforeStartAnOrderAndAFillAreReleasedInPlace) {
    core::EventLoop loop;
    model::Order* order = loop.acquire_order();
    loop.release_order(order);
    loop.put_order(model::Order{.order_id = "OID"});
    EXPECT_EQ(loop.acquire_order(), order); // put_order took it and gave it back
    model::Fill* fill = loop.acquire_fill();
    loop.release_trade(fill);
    loop.put_fill(model::Fill{.fill_id = "TID"});
    EXPECT_EQ(loop.acquire_fill(), fill);
}

TEST(EventLoopSafety, BeforeStartATaskRunsInlineOnTheCallersThread) {
    core::EventLoop loop;
    EXPECT_TRUE(loop.in_event_thread());
    EXPECT_EQ(loop.on_event_thread([] { return std::this_thread::get_id(); }),
              std::this_thread::get_id());
}

TEST(EventLoopSafety, AfterStartATaskRunsOnTheEventThreadAndReturnsOrRethrows) {
    core::EventLoop loop(nullptr, {}, {}, std::chrono::milliseconds(0));
    loop.start();
    EXPECT_FALSE(loop.in_event_thread());
    const auto [id, value] =
        loop.on_event_thread([] { return std::pair(std::this_thread::get_id(), 42); });
    EXPECT_NE(id, std::this_thread::get_id());
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(loop.on_event_thread([&loop] { return loop.in_event_thread(); }));
    EXPECT_THROW(loop.on_event_thread([] { throw std::runtime_error("refused"); }),
                 std::runtime_error);
    EXPECT_EQ(loop.on_event_thread([] { return 7; }), 7); // the loop goes on after a throw
    loop.stop();
}

TEST(EventLoopSafety, ATaskRunsAfterTheTasksPutBeforeIt) {
    core::EventLoop loop(nullptr, {}, {}, std::chrono::milliseconds(0));
    loop.start();
    std::vector<int> ran; // event thread only
    for (int i = 0; i < 100; ++i) {
        loop.put_event(model::Event(model::EventType::Task,
                                    new model::Task([&ran, i] { ran.push_back(i); })));
    }
    const std::vector<int> seen = loop.on_event_thread([&ran] { return ran; });
    ASSERT_EQ(seen.size(), 100U);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(seen[i], i);
    }
    loop.stop();
}

TEST(EventLoopSafety, AfterStopATaskFailsInsteadOfHangingAndPutsAreReleased) {
    core::EventLoop loop(nullptr, {}, {}, std::chrono::milliseconds(0));
    loop.start();
    loop.stop();
    bool ran = false;
    EXPECT_THROW(loop.on_event_thread([&ran] { ran = true; }), std::future_error);
    EXPECT_FALSE(ran);
    model::Order* order = loop.acquire_order();
    loop.release_order(order);
    loop.put_order(model::Order{.order_id = "OID"});
    EXPECT_EQ(loop.acquire_order(), order);
}

} // namespace
