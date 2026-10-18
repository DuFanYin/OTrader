#pragma once

/** Event: a type and a pooled pointer payload, for live and backtest. */

#include "model/object.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <variant>

namespace model {

enum class EventType : uint8_t {
    Timer,
    Order,
    Fill,
    Snapshot,
    Task, // run on the event thread, in turn with the events queued before it
};

/** A Task event's work: the event owns it (new'd) and deletes it once run or dropped. */
using Task = std::function<void()>;

/** All heavy payloads are pointers (pooled); consumer must release on dispatch/drain. */
using EventPayload =
    std::variant<std::monostate, Order*, Fill*, PortfolioSnapshot*, Task*>;

struct Event {
    EventType type = EventType::Timer;
    EventPayload data;

    Event() = default;
    Event(EventType t, EventPayload p = std::monostate{}) : type(t), data(std::move(p)) {}

    /** Clear for reuse in object pool; do not use after reset until reassigned. */
    void reset() {
        type = EventType::Timer;
        data = std::monostate{};
    }
};


} // namespace model
