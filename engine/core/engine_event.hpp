#pragma once

/**
 * EventEngine: the engine's event loop, one of its ports. It brings each event to
 * Engine::process_event in turn. Never started (a backtest), an event is processed in place, on the
 * thread that puts it. Started (live), the events go to a thread of their own (the event thread:
 * the trading state's one writer) through two channels, by their nature, neither taking a lock:
 *   - order updates, fills, tasks and timer ticks join one queue (utilities::MpscQueue), in the
 *     order put, never dropped while the engine runs (no bound; an ERROR once it holds
 *     kQueueAlarm);
 *   - a snapshot replaces its portfolio's last one not yet applied: the newest quotes win.
 * The event thread sleeps on a futex (utilities::Wakeup) until something is put or its next timer
 * tick is due: it keeps the clock, running a tick every interval. The payloads (snapshots, order
 * updates, fills) come from its pools and go back to them once processed.
 */

#include "model/event.hpp"
#include "model/object.hpp"
#include "utilities/base_engine.hpp"
#include "utilities/mpsc_queue.hpp"
#include "utilities/object_pool.hpp"
#include "utilities/wakeup.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace core {

class Engine;

class EventEngine : public utilities::BaseEngine {
  public:
    /** The queue holds this many events not yet processed: logged (ERROR), the event thread is
     * falling behind. */
    static constexpr std::size_t kQueueAlarm = 10'000;
    /** Portfolios with a snapshot slot: a snapshot of one more is dropped (logged). */
    static constexpr std::size_t kMaxPortfolios = 64;

    /** Logs to the engine's log; engine may be null (a test). interval: between the timer ticks
     * once started; zero: none but the Timer events put. */
    explicit EventEngine(Engine* engine,
                         std::chrono::milliseconds interval = std::chrono::seconds(1));
    ~EventEngine();

    EventEngine(const EventEngine&) = delete;
    EventEngine& operator=(const EventEngine&) = delete;

    /** From now on the events go through the channels to the event thread, which keeps the
     * clock. Start and stop from one thread. */
    void start();
    /** Take nothing more; the event thread processes what is queued, then stops. */
    void stop();
    void close() { stop(); }

    /** Processed in place (not started); else queued for the event thread, a snapshot into its
     * portfolio's slot. Its payload goes back to its pool after. Once stopped, released
     * unprocessed. */
    void put_event(const model::Event& event);
    /** A portfolio's quotes: started, the one not yet applied is replaced (the event thread
     * applies only the newest, and before any task put after it); in place otherwise. */
    void put_snapshot(const model::PortfolioSnapshot& snapshot);
    /** True on the thread that processes the events (a Task run there must not wait on one):
     * any thread before start(). */
    [[nodiscard]] bool in_event_thread() const;
    /** The timer ticks the event thread has run. */
    [[nodiscard]] std::uint64_t timer_ticks() const { return timer_ticks_; }

    /** The pools: a producer acquires a payload, fills it and puts its event; it goes back once
     * processed. Null once the pool is closed (the event is dropped). */
    model::PortfolioSnapshot* acquire_snapshot() { return snapshot_pool_.acquire(); }
    void release_snapshot(model::PortfolioSnapshot* p) { snapshot_pool_.release(p); }
    model::OrderData* acquire_order() { return order_pool_.acquire(); }
    void release_order(model::OrderData* p) { order_pool_.release(p); }
    model::TradeData* acquire_trade() { return trade_pool_.acquire(); }
    void release_trade(model::TradeData* p) { trade_pool_.release(p); }

    /** Drop an event no one will process (no payload slot: the engine is closing), releasing its
     * payload; logs it (ERROR). */
    void drop(model::Event& event, std::string_view why);

  private:
    /** An event in the queue: the pool's object, linked through next. */
    struct QueuedEvent {
        model::Event event;
        std::atomic<QueuedEvent*> next{nullptr};

        QueuedEvent() = default;
        QueuedEvent(const QueuedEvent&) = delete;
        /** The pool resets an object by assignment: the event, not the link. */
        QueuedEvent& operator=(QueuedEvent&& other) noexcept {
            event = std::move(other.event);
            return *this;
        }
    };

    /** A portfolio's newest snapshot not applied yet, and a spare one to fill the next into: the
     * buffers go round (filled, applied, filled again), so a snapshot allocates nothing once
     * warm. Any thread puts; only the event thread takes. */
    struct alignas(64) SnapshotSlot {
        std::atomic<const std::string*> portfolio{nullptr}; // whose: set once, by the first put
        std::atomic<model::PortfolioSnapshot*> latest{nullptr};
        std::atomic<model::PortfolioSnapshot*> spare{nullptr};
    };

    /** The portfolio's slot (taken by its first snapshot); null when every slot is another's. */
    SnapshotSlot* slot_of(const std::string& portfolio);
    /** The snapshot becomes its slot's newest; the one it replaces, never applied, the spare. */
    void publish(SnapshotSlot& slot, model::PortfolioSnapshot* snapshot);
    /** The slot keeps a snapshot done with as its spare (the spare before goes to the pool). */
    void keep_spare(SnapshotSlot& slot, model::PortfolioSnapshot* snapshot);
    /** A slot has a snapshot not applied yet (event thread). */
    [[nodiscard]] bool snapshots_waiting() const;
    /** Give the event's payload back to its pool (a task: delete it). */
    void release_event_payload(model::Event* e);
    /** Process the queued event and give it back. */
    void process(QueuedEvent* queued);
    /** Apply each portfolio's newest snapshot put since the last time (event thread). */
    void apply_snapshots();
    /** The events queued as it begins, in order; the snapshots first, and again before each
     * task (event thread). */
    void process_queued();
    void run();

    Engine* engine_; // processes the events
    std::chrono::milliseconds interval_;
    utilities::ObjectPool<QueuedEvent> event_pool_;
    utilities::ObjectPool<model::PortfolioSnapshot> snapshot_pool_;
    utilities::ObjectPool<model::OrderData> order_pool_;
    utilities::ObjectPool<model::TradeData> trade_pool_;

    utilities::MpscQueue<QueuedEvent> queue_;
    utilities::Wakeup wakeup_; // the event thread sleeps here
    std::array<SnapshotSlot, kMaxPortfolios> slots_{}; // taken in order, never given back

    // A producer counts itself in (by 2) while it puts; bit 0: stopped. Once stopped, the event
    // thread leaves when no producer is in and the queue is empty.
    std::atomic<std::uint64_t> gate_{0};
    alignas(64) std::atomic<std::uint64_t> queued_{0}; // events put so far
    alignas(64) std::atomic<std::uint64_t> processed_{0}; // of those, processed (event thread)
    std::atomic<bool> queue_alarmed_{false}; // the alarm told, until the queue shrinks

    std::atomic<bool> running_{false};  // between start() and stop()
    std::atomic<bool> started_{false};  // once started, never in place again
    std::atomic<std::thread::id> event_thread_{};
    std::atomic<std::uint64_t> dropped_events_{0};
    std::atomic<std::uint64_t> timer_ticks_{0};
    std::jthread thread_;
};

} // namespace core
