#include "core/event_loop.hpp"
#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <pthread.h>
#include <sched.h>

namespace core {

namespace {

auto event_type_name(model::EventType type) -> std::string_view {
    switch (type) {
    case model::EventType::Timer:
        return "a Timer";
    case model::EventType::Order:
        return "an Order";
    case model::EventType::Fill:
        return "a fill";
    case model::EventType::Snapshot:
        return "a Snapshot";
    case model::EventType::Task:
        return "a Task";
    }
    return "an";
}

constexpr std::uint64_t kStopped = 1;  // gate_'s bit 0
constexpr std::uint64_t kProducer = 2; // a producer in gate_

} // namespace

EventLoop::EventLoop(utilities::Logger* logger, Handler handler, Report report,
                     std::chrono::milliseconds interval)
    : log_(logger, "EventLoop"), handler_(std::move(handler)), report_(std::move(report)),
      interval_(interval) {}

EventLoop::~EventLoop() {
    EventLoop::stop();
    for (SnapshotSlot& slot : slots_) {
        delete slot.portfolio.load(std::memory_order_relaxed); // its snapshots go with the pool
    }
}

void EventLoop::configure_from_environment() {
    if (const char* spin = std::getenv("OPTRA_EVENT_SPIN_US"); spin != nullptr && *spin != '\0') {
        set_spin(std::chrono::microseconds(std::strtol(spin, nullptr, 10)));
    }
    if (const char* cpu = std::getenv("OPTRA_EVENT_CPU"); cpu != nullptr && *cpu != '\0') {
        set_cpu(static_cast<int>(std::strtol(cpu, nullptr, 10)));
    }
}

void EventLoop::start() {
    bool stopped = false;
    if (!running_.compare_exchange_strong(stopped, true, std::memory_order_acq_rel)) {
        return;
    }
    gate_.store(0, std::memory_order_release);
    started_.store(true, std::memory_order_release);
    thread_ = std::jthread([this] { run(); });
}

void EventLoop::stop() {
    bool running = true;
    if (!running_.compare_exchange_strong(running, false, std::memory_order_acq_rel)) {
        return;
    }
    gate_.fetch_or(kStopped, std::memory_order_acq_rel); // put_event takes nothing more
    wakeup_.wake();
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) {
        thread_.join(); // it processes what was queued, then returns
    }
}

void EventLoop::put_event(const model::Event& event) {
    if (!started_.load(std::memory_order_acquire)) { // in place, on this thread
        model::Event e = event;
        dispatch(e);
        release_event_payload(e);
        return;
    }
    if (event.type == model::EventType::Snapshot) { // into its portfolio's slot, as it is
        model::Event e = event;
        if (auto* p = std::get_if<model::PortfolioSnapshot*>(&e.data); p != nullptr && *p != nullptr) {
            if (SnapshotSlot* slot = slot_of((*p)->portfolio_name)) {
                publish(*slot, std::exchange(*p, nullptr));
                return;
            }
            drop(e, std::format("more than {} portfolios", kMaxPortfolios));
        }
        return;
    }
    QueuedEvent* queued = event_pool_.acquire();
    if (queued == nullptr) {
        drop(event, "no free event slot");
        return;
    }
    queued->event = event;
    queued->queued_tsc = utilities::tsc_now();
    if ((gate_.fetch_add(kProducer, std::memory_order_acq_rel) & kStopped) != 0) {
        gate_.fetch_sub(kProducer, std::memory_order_release); // stopped: nothing processes it
        release_event_payload(queued->event);
        event_pool_.release(queued);
        return;
    }
    const std::uint64_t put = queued_.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint64_t processed = processed_.load(std::memory_order_relaxed); // may be past put
    const std::uint64_t waiting = put > processed ? put - processed : 0;
    queue_.push(queued);
    wakeup_.wake();
    gate_.fetch_sub(kProducer, std::memory_order_release);
    if (waiting >= kQueueAlarm && !queue_alarmed_.exchange(true, std::memory_order_relaxed)) {
        log_.write_log(std::format("{} events queued: the event thread is falling behind (none"
                                   " is dropped)",
                                   waiting),
                       utilities::ERROR);
    }
}

template <typename T>
void EventLoop::put_pooled(model::EventType type, T* slot, const T& data) {
    if (slot == nullptr) {
        drop(model::Event(type), "its pool is closed");
        return;
    }
    *slot = data;
    put_event(model::Event(type, slot));
}

void EventLoop::put_order(const model::Order& order) {
    put_pooled(model::EventType::Order, acquire_order(), order);
}

void EventLoop::put_fill(const model::Fill& fill) {
    put_pooled(model::EventType::Fill, acquire_fill(), fill);
}

void EventLoop::put_snapshot(const model::PortfolioSnapshot& snapshot) {
    if (!started_.load(std::memory_order_acquire)) { // in place, through a pooled copy as any event
        model::PortfolioSnapshot* p = snapshot_pool_.acquire();
        if (p == nullptr) {
            drop(model::Event(model::EventType::Snapshot), "no free snapshot slot");
            return;
        }
        *p = snapshot;
        put_event(model::Event(model::EventType::Snapshot, p));
        return;
    }
    SnapshotSlot* slot = slot_of(snapshot.portfolio_name);
    if (slot == nullptr) {
        drop(model::Event(model::EventType::Snapshot),
             std::format("more than {} portfolios", kMaxPortfolios));
        return;
    }
    model::PortfolioSnapshot* p = slot->spare.exchange(nullptr, std::memory_order_acquire);
    if (p == nullptr) {
        p = snapshot_pool_.acquire();
        if (p == nullptr) {
            drop(model::Event(model::EventType::Snapshot), "no free snapshot slot");
            return;
        }
    }
    *p = snapshot; // the spare's buffers are reused
    publish(*slot, p);
}

auto EventLoop::slot_of(std::string_view portfolio) -> SnapshotSlot* {
    for (SnapshotSlot& slot : slots_) {
        const std::string* owner = slot.portfolio.load(std::memory_order_acquire);
        if (owner == nullptr) {
            auto mine = std::make_unique<std::string>(portfolio);
            if (slot.portfolio.compare_exchange_strong(owner, mine.get(), std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
                mine.release(); // NOLINT(bugprone-unused-return-value): the slot owns it
                return &slot;
            }
            // Another portfolio's first snapshot took it: owner is that portfolio.
        }
        if (*owner == portfolio) {
            return &slot;
        }
    }
    return nullptr;
}

void EventLoop::publish(SnapshotSlot& slot, model::PortfolioSnapshot* snapshot) {
    if ((gate_.load(std::memory_order_acquire) & kStopped) != 0) {
        snapshot_pool_.release(snapshot);
        return;
    }
    if (model::PortfolioSnapshot* replaced = slot.latest.exchange(snapshot, std::memory_order_acq_rel)) {
        keep_spare(slot, replaced);
    }
    wakeup_.wake();
}

void EventLoop::keep_spare(SnapshotSlot& slot, model::PortfolioSnapshot* snapshot) {
    if (model::PortfolioSnapshot* old = slot.spare.exchange(snapshot, std::memory_order_acq_rel)) {
        snapshot_pool_.release(old);
    }
}

auto EventLoop::snapshots_waiting() const -> bool {
    for (const SnapshotSlot& slot : slots_) {
        if (slot.portfolio.load(std::memory_order_relaxed) == nullptr) {
            return false; // the slots are taken in order
        }
        if (slot.latest.load(std::memory_order_relaxed) != nullptr) {
            return true;
        }
    }
    return false;
}

auto EventLoop::in_event_thread() const -> bool {
    return !started_.load(std::memory_order_acquire) ||
           std::this_thread::get_id() == event_thread_.load(std::memory_order_relaxed);
}

void EventLoop::dispatch(const model::Event& event) {
    if (event.type == model::EventType::Task) { // the loop's own: on_event_thread
        if (const auto* task = std::get_if<model::Task*>(&event.data); task && *task) {
            (**task)();
        }
    } else if (handler_) {
        handler_(event);
    }
}

void EventLoop::process(QueuedEvent* queued) {
    dispatch(queued->event);
    if (queued->queued_tsc != 0) {
        const std::uint64_t ns = utilities::tsc_ns(queued->queued_tsc, utilities::tsc_now());
        switch (queued->event.type) {
        case model::EventType::Order:
            order_put_to_done_.record(ns);
            break;
        case model::EventType::Fill:
            trade_put_to_done_.record(ns);
            break;
        case model::EventType::Task:
            task_put_to_done_.record(ns);
            break;
        default:
            break;
        }
    }
    release_event_payload(queued->event);
    event_pool_.release(queued);
}

void EventLoop::apply_snapshots() {
    for (SnapshotSlot& slot : slots_) {
        if (slot.portfolio.load(std::memory_order_relaxed) == nullptr) {
            return;
        }
        if (slot.latest.load(std::memory_order_relaxed) == nullptr) {
            continue;
        }
        model::PortfolioSnapshot* snapshot = slot.latest.exchange(nullptr, std::memory_order_acquire);
        if (snapshot == nullptr) {
            continue;
        }
        if (snapshot->quoted_tsc != 0) {
            quote_to_applied_.record(utilities::tsc_ns(snapshot->quoted_tsc, utilities::tsc_now()));
            applying_quoted_tsc_ = snapshot->quoted_tsc; // orders sent while it is processed
        }
        if (handler_) {
            handler_(model::Event(model::EventType::Snapshot, snapshot));
        }
        applying_quoted_tsc_ = 0;
        keep_spare(slot, snapshot);
    }
}

void EventLoop::process_queued() {
    apply_snapshots();
    std::uint64_t processed = processed_.load(std::memory_order_relaxed);
    const std::uint64_t until = queued_.load(std::memory_order_acquire);
    while (processed < until) {
        QueuedEvent* queued = queue_.pop();
        if (queued == nullptr) {
            if (queue_.empty()) {
                break; // counted, not pushed yet: the next round's
            }
            std::this_thread::yield(); // a push between its exchange and its link
            continue;
        }
        processed_.store(++processed, std::memory_order_relaxed);
        if (queued->event.type == model::EventType::Task) {
            apply_snapshots(); // a task sees every snapshot put before it
        }
        process(queued);
    }
    if (queue_alarmed_.load(std::memory_order_relaxed) &&
        queued_.load(std::memory_order_relaxed) - processed < kQueueAlarm / 2) {
        queue_alarmed_.store(false, std::memory_order_relaxed);
    }
}

void EventLoop::run() {
    using Clock = utilities::Wakeup::Clock;
    event_thread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    if (cpu_ >= 0) {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(cpu_, &cpus);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus) != 0) {
            log_.write_log(std::format("The event thread could not be pinned to core {}", cpu_),
                           utilities::WARNING);
        }
    }
    const bool ticks = interval_ > std::chrono::milliseconds::zero();
    Clock::time_point next_tick = Clock::now() + interval_;
    Clock::time_point next_report = Clock::now() + kLatencyReportInterval;
    for (;;) {
        wakeup_.wait_until(ticks ? next_tick : Clock::time_point::max(), [this] {
            return (gate_.load(std::memory_order_relaxed) & kStopped) != 0 || !queue_.empty() ||
                   snapshots_waiting();
        });
        const bool stopping = (gate_.load(std::memory_order_acquire) & kStopped) != 0;
        process_queued();
        if (stopping) {
            // The producers in when it stopped still push: theirs too, then it is done.
            while (gate_.load(std::memory_order_acquire) != kStopped || !queue_.empty()) {
                process_queued();
                std::this_thread::yield();
            }
            process_queued();
            log_latency_report();
            return;
        }
        if (ticks && Clock::now() >= next_tick) {
            if (handler_) {
                handler_(model::Event(model::EventType::Timer));
            }
            ++timer_ticks_;
            next_tick += interval_;
            if (Clock::now() >= next_report) {
                log_latency_report();
                next_report += kLatencyReportInterval;
            }
            if (const auto now = Clock::now(); next_tick <= now) {
                next_tick = now + interval_; // fell behind by a whole interval: on from now
            }
        }
    }
}

void EventLoop::log_latency_report() const {
    log_.write_log(report_ ? report_() : latency_report(), utilities::INFO);
}

auto EventLoop::latency_report() const -> std::string {
    std::string out = "Latency (event thread, since start):";
    for (const auto& [what, h] : {std::pair{"quote -> applied", &quote_to_applied_},
                                  std::pair{"quote -> order", &quote_to_order_},
                                  std::pair{"order update put -> processed", &order_put_to_done_},
                                  std::pair{"fill put -> processed", &trade_put_to_done_},
                                  std::pair{"command put -> run", &task_put_to_done_}}) {
        if (h->summary().count > 0) {
            out += "\n  " + h->line(what);
        }
    }
    return out;
}

void EventLoop::drop(model::Event event, std::string_view why) {
    const std::uint64_t dropped = ++dropped_events_;
    // A lost order update or fill corrupts a holding: log each. Snapshots are superseded anyway:
    // log the first and every thousandth.
    if (event.type != model::EventType::Snapshot || dropped == 1 || dropped % 1000 == 0) {
        log_.write_log(std::format("Dropped {} event: {} ({} dropped so far)",
                                   event_type_name(event.type), why, dropped),
                       utilities::ERROR);
    }
    release_event_payload(event);
}

void EventLoop::release_event_payload(model::Event& event) {
    if (auto* snapshot = std::get_if<model::PortfolioSnapshot*>(&event.data)) {
        if (*snapshot != nullptr) {
            snapshot_pool_.release(*snapshot);
        }
    } else if (auto* order = std::get_if<model::Order*>(&event.data)) {
        if (*order != nullptr) {
            order_pool_.release(*order);
        }
    } else if (auto* fill = std::get_if<model::Fill*>(&event.data)) {
        if (*fill != nullptr) {
            fill_pool_.release(*fill);
        }
    } else if (auto* task = std::get_if<model::Task*>(&event.data)) {
        delete *task; // NOLINT(cppcoreguidelines-owning-memory): the event owns it
        *task = nullptr;
    }
}

} // namespace core
