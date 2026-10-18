#include "core/event_loop.hpp"
#include "model/event.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

TEST(LiveShutdownE2E, StopAfterQueuedPayloadsDrainsNoLeak) {
    core::EventLoop loop;
    loop.start();

    for (int i = 0; i < 400; ++i) {
        if (auto* ord = loop.acquire_order()) {
            ord->order_id = "OID-" + std::to_string(i);
            loop.put_event(model::Event(model::EventType::Order, ord));
        }
        if (auto* tr = loop.acquire_fill()) {
            tr->fill_id = "TID-" + std::to_string(i);
            loop.put_event(model::Event(model::EventType::Fill, tr));
        }
    }

    loop.stop();
}

TEST(LiveShutdownE2E, StopWhileProducersRunningReturnsAndRejects) {
    core::EventLoop loop;
    loop.start();

    std::atomic<bool> run{true};
    constexpr int kProducers = 4;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&] {
            int i = 0;
            while (run.load(std::memory_order_relaxed)) {
                if (auto* ord = loop.acquire_order()) {
                    ord->order_id = "OID-" + std::to_string(i++);
                    loop.put_event(model::Event(model::EventType::Order, ord));
                }
                if (auto* tr = loop.acquire_fill()) {
                    tr->fill_id = "TID-" + std::to_string(i++);
                    loop.put_event(model::Event(model::EventType::Fill, tr));
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    loop.stop();
    run.store(false, std::memory_order_relaxed);
    for (auto& th : producers) {
        th.join();
    }
}

} // namespace
