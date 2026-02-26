#pragma once

/**
 * EventEngine = dispatch 控制逻辑：按事件类型与固定顺序决定调用谁。
 * 不持有任何引擎；通过 MainEngine 的 accessor
 * 访问各引擎并调用，执行（send_order/cancel_order/put_log_intent）也通过 Main。 Live 额外：queue +
 * worker thread 处理事件。
 */

#include "../../utilities/portfolio.hpp" // IEventEngine, Event, EventType, OrderRequest, CancelRequest
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace engines {

class MainEngine;

/** Alias so callers can use engines::Event (same as utilities::Event). */
using Event = utilities::Event;

/**
 * process(Event) handles one event (dispatch by type in fixed order).
 * Snapshot: update portfolio via apply_frame (same as backtest).
 * Portfolio structure is built from Contract events (order fixed); Snapshot only updates
 * prices/Greeks. set_main_engine required for Timer/Order/Trade/Contract/Snapshot.
 */
class EventEngine : public utilities::IEventEngine {
  public:
    explicit EventEngine(int interval = 1);
    ~EventEngine() override;

    EventEngine(const EventEngine&) = delete;
    EventEngine& operator=(const EventEngine&) = delete;

    void start() override;
    void stop() override;

    std::string put_intent_send_order(const utilities::OrderRequest& req) override;
    void put_intent_cancel_order(const utilities::CancelRequest& req) override;
    void put_intent_log(const utilities::LogData& log) override;
    void put_event(const utilities::Event& event) override;

    void set_main_engine(MainEngine* main_engine) { main_engine_ = main_engine; }

    void put(const utilities::Event& event);
    void put(utilities::Event&& event);
    uint64_t register_handler(utilities::EventType,
                              std::function<void(const utilities::Event&)>) override {
        return 0;
    }
    void unregister_handler(utilities::EventType, uint64_t) override {}

  private:
    void dispatch_snapshot(const utilities::Event& event);
    void dispatch_timer();
    void dispatch_order(const utilities::Event& event);
    void dispatch_trade(const utilities::Event& event);
    void dispatch_contract(const utilities::Event& event);

    void run();
    void run_timer();
    void process(const utilities::Event& event);

    MainEngine* main_engine_ = nullptr;
    int interval_ = 1;
    std::queue<utilities::Event> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<bool> active_{false};
    std::thread thread_;
    std::thread timer_thread_;
};

} // namespace engines
