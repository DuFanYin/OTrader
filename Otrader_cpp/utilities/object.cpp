#include "object.hpp"
#include <algorithm>

namespace utilities {

void ChainMarketData::add_option(const OptionMarketData& option_data) {
    options[option_data.symbol] = option_data;
    if (underlying_last == 0.0 && (underlying_bid > 0 || underlying_ask > 0)) {
        if (underlying_bid > 0 && underlying_ask > 0) {
            underlying_last = (underlying_bid + underlying_ask) / 2.0;
        } else {
            underlying_last = underlying_bid > 0 ? underlying_bid : underlying_ask;
        }
    }
}

auto OrderData::is_active() const -> bool { return is_active_status(status); }

auto OrderData::create_cancel_request() const -> CancelRequest {
    CancelRequest req;
    req.orderid = orderid;
    req.symbol = symbol;
    req.exchange = exchange;
    req.is_combo = is_combo;
    req.legs = legs;
    return req;
}

auto OrderRequest::create_order_data(const std::string& orderid,
                                     const std::string& gateway_name) const -> OrderData {
    OrderData order;
    order.gateway_name = gateway_name;
    order.symbol = symbol;
    order.exchange = exchange;
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

void ComboPositionData::clear_fields() {
    BasePosition::clear_fields();
    for (auto& leg : legs) {
        leg.clear_fields();
    }
}

} // namespace utilities
