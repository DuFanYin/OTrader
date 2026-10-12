#include "object.hpp"
#include <algorithm>

namespace model {

auto OrderData::is_active() const -> bool { return is_active_status(status); }

auto OrderData::create_cancel_request() const -> CancelRequest {
    CancelRequest req;
    req.orderid = orderid;
    req.symbol = symbol;
    req.is_combo = is_combo;
    req.legs = legs;
    return req;
}

auto OrderRequest::create_order_data(const std::string& orderid) const -> OrderData {
    OrderData order;
    order.symbol = symbol;
    order.orderid = orderid;
    order.trading_class = trading_class;
    order.type = type;
    order.direction = direction;
    order.combo_type = combo_type;
    order.price = price;
    order.volume = volume;
    order.reference = reference;
    order.is_combo = is_combo;
    order.legs = legs;
    order.status = Status::SUBMITTING;
    return order;
}

auto BasePosition::current_value() const -> double { return quantity * mid_price * multiplier; }

void BasePosition::clear_fields() {
    if (quantity == 0) {
        avg_cost = 0.0;
        cost_value = 0.0;
        mid_price = 0.0;
        delta = 0.0;
        gamma = 0.0;
        theta = 0.0;
        vega = 0.0;
    }
}

auto OptionPositionData::combo_quantity() const -> int {
    for (const OptionPositionData& leg : legs) {
        if (leg.quantity != 0 && leg.ratio != 0) {
            return leg.quantity / leg.ratio;
        }
    }
    return 0;
}

void OptionPositionData::clear_fields() {
    BasePosition::clear_fields();
    for (auto& leg : legs) {
        leg.clear_fields();
    }
}

} // namespace model
