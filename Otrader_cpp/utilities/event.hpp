#pragma once

/**
 * Event type enum and Event struct with variant payload (shared by live/backtest).
 */

#include "object.hpp"
#include <cstdint>
#include <string>
#include <variant>

namespace utilities {

enum class EventType : uint8_t {
    Timer,
    Order,
    Trade,
    Contract,
    Snapshot,
};

using EventPayload =
    std::variant<std::monostate, OrderData, TradeData, ContractData, PortfolioSnapshot>;

struct Event {
    EventType type = EventType::Timer;
    EventPayload data;

    Event() = default;
    Event(EventType t, EventPayload p = std::monostate{}) : type(t), data(std::move(p)) {}
};

/**
 * Strategy update “event” payload (used by live gRPC stream; not routed via EventEngine for now).
 * Mirrors proto StrategyUpdate: strategy_name/class_name/portfolio/json_payload.
 */
struct StrategyUpdateData {
    std::string strategy_name;
    std::string class_name;
    std::string portfolio;
    std::string json_payload;
};

} // namespace utilities
