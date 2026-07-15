#pragma once

/**
 * Intent: unified variant for put_intent.
 * - SendOrder: send order, returns orderid
 * - CancelOrder: cancel order
 * - Log: log
 */

#include "object.hpp"
#include <cstddef>
#include <string>
#include <variant>

namespace utilities {

enum class IntentType : size_t {
    SendOrder = 0,
    CancelOrder = 1,
    Log = 2,
};

struct IntentSendOrder {
    std::string strategy_name;
    OrderRequest req;
};

struct IntentCancelOrder {
    CancelRequest req;
};

struct IntentLog {
    LogData log;
};

using Intent = std::variant<IntentSendOrder, IntentCancelOrder, IntentLog>;

} // namespace utilities
