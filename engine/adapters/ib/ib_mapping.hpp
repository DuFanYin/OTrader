#pragma once

/** IB's strings for the engine's order status, side and order type, both ways. */

#include "../../model/constant.hpp"
#include <string>
#include <unordered_map>

namespace adapters {

// IB's order status. SUBMITTING is an order IB has not taken yet; PreSubmitted (held: a simulated
// order, or until the open) and PendingCancel are taken, working.
inline model::Status status_from_ib(const std::string& status) {
    static const std::unordered_map<std::string, model::Status> m = {
        {"ApiPending", model::Status::SUBMITTING},
        {"PendingSubmit", model::Status::SUBMITTING},
        {"PreSubmitted", model::Status::NOTTRADED},
        {"Submitted", model::Status::NOTTRADED},
        {"ApiCancelled", model::Status::CANCELLED},
        {"PendingCancel", model::Status::NOTTRADED},
        {"Cancelled", model::Status::CANCELLED},
        {"Filled", model::Status::ALLTRADED},
        {"Inactive", model::Status::REJECTED},
        {"PartiallyFilled", model::Status::PARTTRADED},
    };
    auto it = m.find(status);
    return it != m.end() ? it->second : model::Status::SUBMITTING;
}

// IB's order action.
inline std::string direction_to_ib(model::Direction d) {
    return d == model::Direction::SHORT ? "SELL" : "BUY";
}

// IB's order action, or an execution's side (BOT, SLD).
inline model::Direction direction_from_ib(const std::string& action) {
    return action == "SELL" || action == "SLD" ? model::Direction::SHORT : model::Direction::LONG;
}

// IB's order type.
inline std::string ordertype_to_ib(model::OrderType t) {
    return t == model::OrderType::MARKET ? "MKT" : "LMT";
}

inline model::OrderType ordertype_from_ib(const std::string& t) {
    return t == "MKT" ? model::OrderType::MARKET : model::OrderType::LIMIT;
}

} // namespace adapters
