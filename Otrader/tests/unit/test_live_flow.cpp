/**
 * Live dataflow test — §8.1
 * EventEngine + main queue: put_event(Timer), no market-data polling or broker connection.
 *
 * MainEngine now hosts market-data + gateway in-process; construction loads contracts from the
 * DB and close() tears down the event engine, gateway pump, and DB. The timeout guards against
 * a hang in DB/engine teardown.
 */
#include "runtime/live/engine_main.hpp"
#include "utilities/event.hpp"
#include <atomic>
#include <chrono>
#include <exception>
#include <gtest/gtest.h>
#include <thread>

namespace {

using namespace utilities;

constexpr std::chrono::seconds kTimeout{25};

// Runs fn() in a thread; if it doesn't complete within kTimeout, fails the test and detaches.
template <typename Fn> void run_with_timeout(Fn&& fn) {
    std::atomic<bool> done{false};
    std::exception_ptr ep;
    std::thread t([&] {
        try {
            fn();
            done = true;
        } catch (...) {
            ep = std::current_exception();
            done = true;
        }
    });
    auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (!done && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (ep) {
        if (t.joinable())
            t.join();
        std::rethrow_exception(ep);
    }
    if (!done) {
        t.detach();
        GTEST_FAIL() << "LiveFlow timed out (close() may block on DB/engine teardown); test aborted.";
    } else if (t.joinable()) {
        t.join();
    }
}

TEST(LiveFlow, PutTimerEventAndClose) {
    run_with_timeout([] {
        engines::MainEngine main_engine;
        ASSERT_NE(main_engine.event_engine(), nullptr);
        main_engine.put_event(Event(EventType::Timer));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        main_engine.close();
    });
}

TEST(LiveFlow, PutMultipleTimerEvents) {
    run_with_timeout([] {
        engines::MainEngine main_engine;
        ASSERT_NE(main_engine.event_engine(), nullptr);
        for (int i = 0; i < 5; ++i)
            main_engine.put_event(Event(EventType::Timer));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        main_engine.close();
    });
}

} // namespace
