/** Wakeup: sleeping until work or a deadline, woken early, and polling before it sleeps. */
#include "utilities/wakeup.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace {

using namespace std::chrono_literals;
using Clock = utilities::Wakeup::Clock;

TEST(Wakeup, WithoutWorkItSleepsUntilTheDeadline) {
    utilities::Wakeup wakeup;
    const auto start = Clock::now();
    wakeup.wait_until(start + 20ms, [] { return false; });
    const auto slept = Clock::now() - start;
    EXPECT_GE(slept, 20ms);
    EXPECT_LT(slept, 1s);
}

TEST(Wakeup, WorkThereAlreadyReturnsAtOnce) {
    utilities::Wakeup wakeup;
    const auto start = Clock::now();
    wakeup.wait_until(Clock::time_point::max(), [] { return true; });
    EXPECT_LT(Clock::now() - start, 100ms);
}

TEST(Wakeup, AWakeEndsTheSleepWellBeforeTheDeadline) {
    utilities::Wakeup wakeup;
    std::atomic<bool> work{false};
    std::thread waker([&] {
        std::this_thread::sleep_for(10ms);
        work = true;
        wakeup.wake();
    });
    const auto start = Clock::now();
    wakeup.wait_until(Clock::time_point::max(), [&] { return work.load(); });
    waker.join();
    EXPECT_TRUE(work.load());
    EXPECT_LT(Clock::now() - start, 5s);
}

TEST(Wakeup, SpinningItSeesWorkThatCameWithoutAWake) {
    utilities::Wakeup wakeup;
    wakeup.set_spin(2s); // polls for up to two seconds before it would sleep
    std::atomic<bool> work{false};
    std::thread producer([&] {
        std::this_thread::sleep_for(5ms);
        work = true; // no wake(): only polling finds it
    });
    const auto start = Clock::now();
    wakeup.wait_until(Clock::time_point::max(), [&] { return work.load(); });
    producer.join();
    EXPECT_TRUE(work.load());
    EXPECT_LT(Clock::now() - start, 1s);
}

TEST(Wakeup, SpinningNoLongerThanTheDeadlineThenSleepingToIt) {
    utilities::Wakeup wakeup;
    wakeup.set_spin(1ms);
    const auto start = Clock::now();
    wakeup.wait_until(start + 20ms, [] { return false; });
    const auto waited = Clock::now() - start;
    EXPECT_GE(waited, 20ms);
    EXPECT_LT(waited, 1s);

    wakeup.set_spin(1s); // the deadline comes first: polling stops at it
    const auto again = Clock::now();
    wakeup.wait_until(again + 10ms, [] { return false; });
    EXPECT_LT(Clock::now() - again, 500ms);
}

TEST(Wakeup, AWakeBetweenItsCheckAndItsSleepIsNotLost) {
    using Clock = utilities::Wakeup::Clock;
    utilities::Wakeup wakeup;
    for (int round = 0; round < 200; ++round) {
        std::atomic<bool> work{false};
        std::thread waker([&] {
            work.store(true, std::memory_order_release);
            wakeup.wake();
        });
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!work.load(std::memory_order_acquire) && Clock::now() < deadline) {
            wakeup.wait_until(deadline, [&] { return work.load(std::memory_order_acquire); });
        }
        waker.join();
        ASSERT_LT(Clock::now(), deadline) << "round " << round << ": a wake was lost";
    }
}

} // namespace
