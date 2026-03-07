#pragma once

/** EventEngine: dispatch by type/order; queue + worker thread. */

#include "../../utilities/base_engine.hpp"
#include "../../utilities/intent.hpp"
#include "../../utilities/portfolio.hpp" // Event, EventType, OrderRequest, CancelRequest, LogData
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>

namespace engines {

class MainEngine;

/** Alias so callers can use engines::Event (same as utilities::Event). */
using Event = utilities::Event;

/** process(Event): dispatch by type; Snapshot→apply_frame. */
class EventEngine : public utilities::BaseEngine {
  public:
    explicit EventEngine(utilities::MainEngine* main, int interval = 1);
    ~EventEngine() override;

    EventEngine(const EventEngine&) = delete;
    EventEngine& operator=(const EventEngine&) = delete;

    void start();
    void stop();

    void close() override;

    /** Intent entry; SendOrder→orderid, others→nullopt. */
    std::optional<std::string> put_intent(const utilities::Intent& intent);
    void put_event(const utilities::Event& event);

    void put(const utilities::Event& event);
    void put(utilities::Event&& event);
    uint64_t register_handler(utilities::EventType, std::function<void(const utilities::Event&)>) {
        return 0;
    }
    void unregister_handler(utilities::EventType, uint64_t) {}

  private:
    void dispatch_snapshot(const utilities::Event& event);
    void dispatch_timer();
    void dispatch_order(const utilities::Event& event);
    void dispatch_trade(const utilities::Event& event);
    void run(const std::stop_token& st);
    void run_timer(const std::stop_token& st);
    void process(const utilities::Event& event);

    int interval_ = 1;
    std::queue<utilities::Event> queue_;
    std::mutex queue_mutex_;
    std::condition_variable_any queue_cv_;
    std::atomic<bool> active_{false};
    std::jthread thread_;
    std::jthread timer_thread_;
};

} // namespace engines
