#include "core/engine_event.hpp"
#include "core/engine_main.hpp"
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace core {

namespace {

auto event_type_name(model::EventType type) -> std::string_view {
    switch (type) {
    case model::EventType::Timer:
        return "a Timer";
    case model::EventType::Order:
        return "an Order";
    case model::EventType::Trade:
        return "a Trade";
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

EventEngine::EventEngine(Engine* engine, std::chrono::milliseconds interval)
    : BaseEngine(engine != nullptr ? engine->log_engine() : nullptr, "Event"), engine_(engine),
      interval_(interval) {}

EventEngine::~EventEngine() {
    EventEngine::stop();
    for (SnapshotSlot& slot : slots_) {
        delete slot.portfolio.load(std::memory_order_relaxed); // its snapshots go with the pool
    }
}

void EventEngine::start() {
    bool stopped = false;
    if (!running_.compare_exchange_strong(stopped, true, std::memory_order_acq_rel)) {
        return;
    }
    gate_.store(0, std::memory_order_release);
    started_.store(true, std::memory_order_release);
    thread_ = std::jthread([this] { run(); });
}

void EventEngine::stop() {
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

void EventEngine::put_event(const model::Event& event) {
    if (!started_.load(std::memory_order_acquire)) { // in place, on this thread
        model::Event e = event;
        if (engine_ != nullptr) {
            engine_->process_event(e);
        }
        release_event_payload(&e);
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
        model::Event dropped = event;
        drop(dropped, "no free event slot");
        return;
    }
    queued->event = event;
    if ((gate_.fetch_add(kProducer, std::memory_order_acq_rel) & kStopped) != 0) {
        gate_.fetch_sub(kProducer, std::memory_order_release); // stopped: nothing processes it
        release_event_payload(&queued->event);
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
        write_log(std::format("{} events queued: the event thread is falling behind (none is"
                              " dropped)",
                              waiting),
                  utilities::ERROR);
    }
}

void EventEngine::put_snapshot(const model::PortfolioSnapshot& snapshot) {
    if (!started_.load(std::memory_order_acquire)) { // in place, through a pooled copy as any event
        model::PortfolioSnapshot* p = snapshot_pool_.acquire();
        if (p == nullptr) {
            model::Event dropped(model::EventType::Snapshot);
            drop(dropped, "no free snapshot slot");
            return;
        }
        *p = snapshot;
        put_event(model::Event(model::EventType::Snapshot, p));
        return;
    }
    SnapshotSlot* slot = slot_of(snapshot.portfolio_name);
    if (slot == nullptr) {
        model::Event dropped(model::EventType::Snapshot);
        drop(dropped, std::format("more than {} portfolios", kMaxPortfolios));
        return;
    }
    model::PortfolioSnapshot* p = slot->spare.exchange(nullptr, std::memory_order_acquire);
    if (p == nullptr) {
        p = snapshot_pool_.acquire();
        if (p == nullptr) {
            model::Event dropped(model::EventType::Snapshot);
            drop(dropped, "no free snapshot slot");
            return;
        }
    }
    *p = snapshot; // the spare's buffers are reused
    publish(*slot, p);
}

auto EventEngine::slot_of(const std::string& portfolio) -> SnapshotSlot* {
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

void EventEngine::publish(SnapshotSlot& slot, model::PortfolioSnapshot* snapshot) {
    if ((gate_.load(std::memory_order_acquire) & kStopped) != 0) {
        snapshot_pool_.release(snapshot);
        return;
    }
    if (model::PortfolioSnapshot* replaced = slot.latest.exchange(snapshot, std::memory_order_acq_rel)) {
        keep_spare(slot, replaced);
    }
    wakeup_.wake();
}

void EventEngine::keep_spare(SnapshotSlot& slot, model::PortfolioSnapshot* snapshot) {
    if (model::PortfolioSnapshot* old = slot.spare.exchange(snapshot, std::memory_order_acq_rel)) {
        snapshot_pool_.release(old);
    }
}

auto EventEngine::snapshots_waiting() const -> bool {
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

auto EventEngine::in_event_thread() const -> bool {
    return !started_.load(std::memory_order_acquire) ||
           std::this_thread::get_id() == event_thread_.load(std::memory_order_relaxed);
}

void EventEngine::process(QueuedEvent* queued) {
    if (engine_ != nullptr) {
        engine_->process_event(queued->event);
    }
    release_event_payload(&queued->event);
    event_pool_.release(queued);
}

void EventEngine::apply_snapshots() {
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
        if (engine_ != nullptr) {
            engine_->process_event(model::Event(model::EventType::Snapshot, snapshot));
        }
        keep_spare(slot, snapshot);
    }
}

void EventEngine::process_queued() {
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

void EventEngine::run() {
    using Clock = utilities::Wakeup::Clock;
    event_thread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    const bool ticks = interval_ > std::chrono::milliseconds::zero();
    Clock::time_point next_tick = Clock::now() + interval_;
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
            return;
        }
        if (ticks && Clock::now() >= next_tick) {
            if (engine_ != nullptr) {
                engine_->process_event(model::Event(model::EventType::Timer));
            }
            ++timer_ticks_;
            next_tick += interval_;
            if (const auto now = Clock::now(); next_tick <= now) {
                next_tick = now + interval_; // fell behind by a whole interval: on from now
            }
        }
    }
}

void EventEngine::drop(model::Event& event, std::string_view why) {
    const std::uint64_t dropped = ++dropped_events_;
    // An order update or a fill lost is a holding gone wrong: each is told. A snapshot is
    // superseded by the next: the first and every thousandth are.
    if (event.type != model::EventType::Snapshot || dropped == 1 || dropped % 1000 == 0) {
        write_log(std::format("Dropped {} event: {} ({} dropped so far)",
                              event_type_name(event.type), why, dropped),
                  utilities::ERROR);
    }
    release_event_payload(&event);
}

void EventEngine::release_event_payload(model::Event* e) {
    if (e == nullptr) {
        return;
    }
    if (auto* snapshot = std::get_if<model::PortfolioSnapshot*>(&e->data)) {
        if (*snapshot != nullptr) {
            snapshot_pool_.release(*snapshot);
        }
    } else if (auto* order = std::get_if<model::OrderData*>(&e->data)) {
        if (*order != nullptr) {
            order_pool_.release(*order);
        }
    } else if (auto* trade = std::get_if<model::TradeData*>(&e->data)) {
        if (*trade != nullptr) {
            trade_pool_.release(*trade);
        }
    } else if (auto* task = std::get_if<model::Task*>(&e->data)) {
        delete *task; // NOLINT(cppcoreguidelines-owning-memory): the event owns it
        *task = nullptr;
    }
}

} // namespace core
