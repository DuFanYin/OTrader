#pragma once

/**
 * EventLoop: runs Tasks itself and hands other events to its handler. Not started
 * (backtest): processed inline on the caller's thread. Started (live): handed lock-free to the event thread, the trading state's
 * single writer:
 *   - orders, fills, tasks and timer ticks go through an unbounded FIFO, never dropped (ERROR
 *     past kQueueAlarm);
 *   - a snapshot replaces its portfolio's unapplied one: the newest quotes win.
 * The event thread sleeps on a futex until work arrives or the next timer tick is due. Payloads
 * come from pools and return once processed.
 */

#include "model/event.hpp"
#include "model/object.hpp"
#include "utilities/log_source.hpp"
#include "utilities/latency.hpp"
#include "utilities/mpsc_queue.hpp"
#include "utilities/object_pool.hpp"
#include "utilities/wakeup.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace core {

class EventLoop {
  public:
    using Handler = std::function<void(const model::Event&)>;
    using Report = std::function<std::string()>;

    /** Backlog that logs an ERROR: the event thread is falling behind. */
    static constexpr std::size_t kQueueAlarm = 10'000;
    /** Snapshots of portfolios beyond this are dropped (logged). */
    static constexpr std::size_t kMaxPortfolios = 64;

    /** handler empty (tests): events other than Tasks are only released. report: the latency
     * report logged periodically and at stop; empty: latency_report(). interval zero: only
     * explicitly put Timer events. */
    explicit EventLoop(utilities::Logger* logger = nullptr, Handler handler = {},
                       Report report = {},
                       std::chrono::milliseconds interval = std::chrono::seconds(1));
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /** Before start(): spin before sleeping, and pin to a core (-1: any). Spinning burns a core
     * but takes the futex wake-up (about 11 us per hand-off) off the hot path. */
    void set_spin(std::chrono::microseconds spin) { wakeup_.set_spin(spin); }
    void set_cpu(int cpu) { cpu_ = cpu; }
    /** Before start(). */
    void set_interval(std::chrono::milliseconds interval) { interval_ = interval; }
    /** $OPTRA_EVENT_SPIN_US, $OPTRA_EVENT_CPU. */
    void configure_from_environment();
    /** Start and stop from one thread. */
    void start();
    /** Drains what is queued, then stops. */
    void stop();
    void close() { stop(); }

    /** Takes ownership of the payload. Once stopped, released unprocessed. */
    void put_event(const model::Event& event);
    /** Started: replaces the unapplied one; applied before any task put after it. */
    void put_snapshot(const model::PortfolioSnapshot& snapshot);
    /** Copied into the pool; dropped (logged) once the pool is closed. */
    void put_order(const model::Order& order);
    void put_fill(const model::Fill& fill);
    /** Runs fn here after the events put before it and returns (or rethrows) its result; inline
     * when already here. The only way other threads (gRPC) touch trading state. */
    template <typename Fn> auto on_event_thread(Fn&& fn) -> std::invoke_result_t<Fn&> {
        using Result = std::invoke_result_t<Fn&>;
        if (in_event_thread()) {
            return fn();
        }
        // If the task is dropped unrun, the broken promise makes get() throw instead of hang.
        auto work = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
        std::future<Result> done = work->get_future();
        put_event(model::Event(model::EventType::Task,
                               new model::Task([work = std::move(work)] { (*work)(); })));
        return done.get();
    }

    /** A Task here must not wait on another. True on any thread before start(). */
    [[nodiscard]] bool in_event_thread() const;
    [[nodiscard]] std::uint64_t timer_ticks() const { return timer_ticks_; }

    /** Null once the pool is closed. */
    model::PortfolioSnapshot* acquire_snapshot() { return snapshot_pool_.acquire(); }
    void release_snapshot(model::PortfolioSnapshot* p) { snapshot_pool_.release(p); }
    model::Order* acquire_order() { return order_pool_.acquire(); }
    void release_order(model::Order* p) { order_pool_.release(p); }
    model::Fill* acquire_fill() { return fill_pool_.acquire(); }
    void release_trade(model::Fill* p) { fill_pool_.release(p); }

    /** Releases the payload and logs an ERROR. */
    void drop(model::Event event, std::string_view why);

    /** Event thread: records quote -> order latency while a live snapshot is processed. */
    void note_order_sent() {
        if (applying_quoted_tsc_ != 0) {
            quote_to_order_.record(utilities::tsc_ns(applying_quoted_tsc_, utilities::tsc_now()));
        }
    }
    /** Any thread. Also logged every kLatencyReportInterval and at stop. */
    [[nodiscard]] std::string latency_report() const;
    static constexpr std::chrono::minutes kLatencyReportInterval{15};

  private:
    template <typename T> void put_pooled(model::EventType type, T* slot, const T& data);
    /** A Task runs here; any other event goes to the handler. */
    void dispatch(const model::Event& event);
    struct QueuedEvent {
        model::Event event;
        std::atomic<QueuedEvent*> next{nullptr};
        std::uint64_t queued_tsc = 0; // put time (TSC)

        QueuedEvent() = default;
        QueuedEvent(const QueuedEvent&) = delete;
        /** Pool reset: assigns the event, not the link. */
        QueuedEvent& operator=(QueuedEvent&& other) noexcept {
            event = std::move(other.event);
            return *this;
        }
    };

    /** Newest unapplied snapshot plus a spare; buffers recycle so nothing allocates once warm.
     * Any thread puts; only the event thread takes. */
    struct alignas(64) SnapshotSlot {
        std::atomic<const std::string*> portfolio{nullptr}; // set once, by the first put
        std::atomic<model::PortfolioSnapshot*> latest{nullptr};
        std::atomic<model::PortfolioSnapshot*> spare{nullptr};
    };

    /** Null when every slot is taken. */
    SnapshotSlot* slot_of(std::string_view portfolio);
    /** The replaced, never-applied snapshot becomes the spare. */
    void publish(SnapshotSlot& slot, model::PortfolioSnapshot* snapshot);
    /** The previous spare goes to the pool. */
    void keep_spare(SnapshotSlot& slot, model::PortfolioSnapshot* snapshot);
    /** Event thread. */
    [[nodiscard]] bool snapshots_waiting() const;
    /** A task is deleted. */
    void release_event_payload(model::Event& event);
    void process(QueuedEvent* queued);
    /** Event thread. */
    void apply_snapshots();
    /** Event thread: snapshots first, and again before each task. */
    void process_queued();
    void run();
    void log_latency_report() const;

    utilities::LogSource log_;
    Handler handler_;
    Report report_;
    std::chrono::milliseconds interval_;
    utilities::ObjectPool<QueuedEvent> event_pool_;
    utilities::ObjectPool<model::PortfolioSnapshot> snapshot_pool_;
    utilities::ObjectPool<model::Order> order_pool_;
    utilities::ObjectPool<model::Fill> fill_pool_;

    utilities::MpscQueue<QueuedEvent> queue_;
    utilities::Wakeup wakeup_;
    std::array<SnapshotSlot, kMaxPortfolios> slots_{}; // taken in order, never given back

    // Producers add 2 while putting; bit 0: stopped. The event thread exits once stopped, no
    // producer is in, and the queue is empty.
    std::atomic<std::uint64_t> gate_{0};
    alignas(64) std::atomic<std::uint64_t> queued_{0};
    alignas(64) std::atomic<std::uint64_t> processed_{0}; // event thread
    std::atomic<bool> queue_alarmed_{false}; // until the queue shrinks

    // Read-mostly, by every put: apart from processed_, which the event thread writes per event.
    alignas(64) std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};  // never inline again once started
    std::atomic<std::thread::id> event_thread_{};
    std::atomic<std::uint64_t> dropped_events_{0};
    std::atomic<std::uint64_t> timer_ticks_{0};
    int cpu_ = -1;

    // Event thread only. quote_to_*: from a snapshot's first quote (pricing and hand-off
    // included); *_put_to_done: queued events by kind.
    alignas(64) utilities::LatencyHistogram quote_to_applied_;
    utilities::LatencyHistogram quote_to_order_;
    utilities::LatencyHistogram order_put_to_done_;
    utilities::LatencyHistogram trade_put_to_done_;
    utilities::LatencyHistogram task_put_to_done_;
    std::uint64_t applying_quoted_tsc_ = 0; // of the snapshot being processed, else 0
    std::jthread thread_;
};

} // namespace core
