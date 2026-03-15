#pragma once

/** EventEngine: dispatch by type/order; queue + worker thread. C++20.
 * Main worker: infrastructure (Snapshot, ExecutionEngine, PositionEngine, HedgeEngine).
 * Strategy worker: dedicated queue + thread for on_timer, process_order, process_trade.
 */

#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/intent.hpp"
#include "../../utilities/mpsc_ring.hpp"
#include "../../utilities/object_pool.hpp"
#include "../../utilities/portfolio.hpp"
#include "../../utilities/spsc_ring.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
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
    void put_event(utilities::Event&& event);

    void put(const utilities::Event& event);
    void put(utilities::Event&& event);

    /** Snapshot pool: producer acquires, fills, put_event(Snapshot, p); consumer releases after
     * dispatch or on drain. */
    utilities::PortfolioSnapshot* acquire_snapshot() { return snapshot_pool_.acquire(); }
    void release_snapshot(utilities::PortfolioSnapshot* p) { snapshot_pool_.release(p); }
    /** Order/Trade pools: same pattern; producer acquires, fills, put_event; consumer releases. */
    utilities::OrderData* acquire_order() { return order_pool_.acquire(); }
    void release_order(utilities::OrderData* p) { order_pool_.release(p); }
    utilities::TradeData* acquire_trade() { return trade_pool_.acquire(); }
    void release_trade(utilities::TradeData* p) { trade_pool_.release(p); }

    uint64_t register_handler(utilities::EventType, std::function<void(const utilities::Event&)>) {
        return 0;
    }
    void unregister_handler(utilities::EventType, uint64_t) {}

  private:
    void dispatch_snapshot(const utilities::Event& event);
    void dispatch_timer();
    void dispatch_order(utilities::Event* event);
    void dispatch_trade(utilities::Event* event);
    void release_event_payload(utilities::Event* e);
    void run(const std::stop_token& st);
    void run_timer(const std::stop_token& st);
    void run_strategy(const std::stop_token& st);
    void process(utilities::Event* event);
    void process_strategy(const utilities::Event& event);

    int interval_ = 1;
    utilities::ObjectPool<utilities::Event> event_pool_;
    utilities::ObjectPool<utilities::PortfolioSnapshot> snapshot_pool_;
    utilities::ObjectPool<utilities::OrderData> order_pool_;
    utilities::ObjectPool<utilities::TradeData> trade_pool_;
    static constexpr size_t kMainRingCap = 512;
    utilities::MpscRing<utilities::Event*, kMainRingCap> queue_ring_;
    std::mutex queue_mutex_;
    std::condition_variable_any queue_cv_;
    static constexpr size_t kStrategyRingCap = 256;
    utilities::SpscRing<utilities::Event*, kStrategyRingCap> strategy_ring_;
    std::mutex strategy_mutex_;
    std::condition_variable_any strategy_cv_;
    std::atomic<bool> active_{false};
    std::jthread thread_;
    std::jthread timer_thread_;
    std::jthread strategy_thread_;
};

} // namespace engines
