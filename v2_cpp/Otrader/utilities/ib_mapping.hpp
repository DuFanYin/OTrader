#pragma once

/**
 * IB <-> VT (platform) constant mappings for gateway.
 * Mirrors utilities/constant.py (STATUS_IB2VT, DIRECTION_VT2IB, etc.).
 */

#include "constant.hpp"
#include <string>
#include <unordered_map>

namespace engines {

// Order status: IB status string -> Status
inline utilities::Status status_ib2vt(const std::string& status) {
    static const std::unordered_map<std::string, utilities::Status> m = {
        {"ApiPending", utilities::Status::SUBMITTING},
        {"PendingSubmit", utilities::Status::SUBMITTING},
        {"PreSubmitted", utilities::Status::SUBMITTING},
        {"Submitted", utilities::Status::NOTTRADED},
        {"ApiCancelled", utilities::Status::CANCELLED},
        {"PendingCancel", utilities::Status::SUBMITTING},
        {"Cancelled", utilities::Status::CANCELLED},
        {"Filled", utilities::Status::ALLTRADED},
        {"Inactive", utilities::Status::REJECTED},
        {"PartiallyFilled", utilities::Status::PARTTRADED},
    };
    auto it = m.find(status);
    return it != m.end() ? it->second : utilities::Status::SUBMITTING;
}

// Direction: VT -> IB action string
inline std::string direction_vt2ib(utilities::Direction d) {
    switch (d) {
    case utilities::Direction::LONG:
        return "BUY";
    case utilities::Direction::SHORT:
        return "SELL";
    default:
        return "BUY";
    }
}

// Direction: IB action string -> VT
inline utilities::Direction direction_ib2vt(const std::string& action) {
    if (action == "BUY" || action == "BOT")
        return utilities::Direction::LONG;
    if (action == "SELL" || action == "SLD")
        return utilities::Direction::SHORT;
    return utilities::Direction::LONG;
}

// Order type: VT -> IB order type string
inline std::string ordertype_vt2ib(utilities::OrderType t) {
    switch (t) {
    case utilities::OrderType::LIMIT:
        return "LMT";
    case utilities::OrderType::MARKET:
        return "MKT";
    default:
        return "LMT";
    }
}

// Order type: IB order type string -> VT
inline utilities::OrderType ordertype_ib2vt(const std::string& t) {
    if (t == "MKT")
        return utilities::OrderType::MARKET;
    return utilities::OrderType::LIMIT;
}

// Exchange: IB exchange string -> VT
inline utilities::Exchange exchange_ib2vt(const std::string& exch) {
    static const std::unordered_map<std::string, utilities::Exchange> m = {
        {"SMART", utilities::Exchange::SMART},   {"NYSE", utilities::Exchange::NYSE},
        {"NASDAQ", utilities::Exchange::NASDAQ}, {"AMEX", utilities::Exchange::AMEX},
        {"CBOE", utilities::Exchange::CBOE},     {"IBKRATS", utilities::Exchange::IBKRATS},
    };
    auto it = m.find(exch);
    return it != m.end() ? it->second : utilities::Exchange::SMART;
}

// Exchange: VT -> IB exchange string
inline std::string exchange_vt2ib(utilities::Exchange e) { return utilities::to_string(e); }

// Product: IB secType -> VT
inline utilities::Product product_ib2vt(const std::string& sec_type) {
    static const std::unordered_map<std::string, utilities::Product> m = {
        {"STK", utilities::Product::EQUITY},  {"OPT", utilities::Product::OPTION},
        {"FOP", utilities::Product::OPTION},  {"IND", utilities::Product::INDEX},
        {"FUT", utilities::Product::FUTURES},
    };
    auto it = m.find(sec_type);
    return it != m.end() ? it->second : utilities::Product::UNKNOWN;
}

// Option type: IB right -> VT
inline utilities::OptionType option_ib2vt(const std::string& right) {
    if (right == "C" || right == "CALL")
        return utilities::OptionType::CALL;
    if (right == "P" || right == "PUT")
        return utilities::OptionType::PUT;
    return utilities::OptionType::CALL;
}

} // namespace engines
