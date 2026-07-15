/** ZMQ Gateway schema: Protobuf serialization. */

#include "zmq_gateway_schema.hpp"
#include "../../proto/zmq_messages.pb.h"
#include "../../utilities/datetime_serialize.hpp"

namespace engines {

namespace {

static void leg_to_proto(const utilities::Leg& leg, otrader::zmq::ZmqLeg* out) {
    out->set_con_id(leg.con_id);
    out->set_exchange(utilities::to_string(leg.exchange));
    out->set_ratio(leg.ratio);
    out->set_direction(utilities::to_string(leg.direction));
    if (leg.price.has_value()) {
        out->set_price(*leg.price);
    }
    if (leg.symbol.has_value()) {
        out->set_symbol(*leg.symbol);
    }
    if (leg.trading_class.has_value()) {
        out->set_trading_class(*leg.trading_class);
    }
}

static utilities::Leg leg_from_proto(const otrader::zmq::ZmqLeg& in) {
    utilities::Leg leg;
    leg.con_id = in.con_id();
    leg.exchange = utilities::from_string_exchange(in.exchange().empty() ? "LOCAL" : in.exchange());
    leg.ratio = in.ratio();
    leg.direction =
        utilities::from_string_direction(in.direction().empty() ? "LONG" : in.direction());
    if (in.price() != 0.0) {
        leg.price = in.price();
    }
    if (!in.symbol().empty()) {
        leg.symbol = in.symbol();
    }
    if (!in.trading_class().empty()) {
        leg.trading_class = in.trading_class();
    }
    return leg;
}

} // namespace

std::string order_serialize(const utilities::OrderData& o) {
    otrader::zmq::ZmqOrderData pb;
    pb.set_gateway_name(o.gateway_name);
    pb.set_symbol(o.symbol);
    pb.set_exchange(utilities::to_string(o.exchange));
    pb.set_orderid(o.orderid);
    if (o.trading_class.has_value()) {
        pb.set_trading_class(*o.trading_class);
    }
    pb.set_type(utilities::to_string(o.type));
    if (o.direction.has_value()) {
        pb.set_direction(utilities::to_string(*o.direction));
    }
    pb.set_price(o.price);
    pb.set_volume(o.volume);
    pb.set_traded(o.traded);
    pb.set_status(utilities::to_string(o.status));
    if (o.datetime.has_value()) {
        pb.set_datetime(utilities::datetime_to_iso_string(*o.datetime));
    }
    pb.set_reference(o.reference);
    pb.set_is_combo(o.is_combo);
    if (o.legs.has_value()) {
        for (const auto& leg : *o.legs) {
            leg_to_proto(leg, pb.add_legs());
        }
    }
    if (o.combo_type.has_value()) {
        pb.set_combo_type(utilities::to_string(*o.combo_type));
    }
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<utilities::OrderData> order_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqOrderData pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    utilities::OrderData o;
    o.gateway_name = pb.gateway_name();
    o.symbol = pb.symbol();
    o.exchange = utilities::from_string_exchange(pb.exchange().empty() ? "LOCAL" : pb.exchange());
    o.orderid = pb.orderid();
    if (!pb.trading_class().empty()) {
        o.trading_class = pb.trading_class();
    }
    o.type = utilities::from_string_ordertype(pb.type().empty() ? "LIMIT" : pb.type());
    if (!pb.direction().empty()) {
        o.direction = utilities::from_string_direction(pb.direction());
    }
    o.price = pb.price();
    o.volume = pb.volume();
    o.traded = pb.traded();
    o.status = utilities::from_string_status(pb.status().empty() ? "SUBMITTING" : pb.status());
    if (!pb.datetime().empty()) {
        o.datetime = utilities::datetime_from_iso_string(pb.datetime());
    }
    o.reference = pb.reference();
    o.is_combo = pb.is_combo();
    for (int i = 0; i < pb.legs_size(); ++i) {
        if (!o.legs) {
            o.legs = std::vector<utilities::Leg>{};
        }
        o.legs->push_back(leg_from_proto(pb.legs(i)));
    }
    if (!pb.combo_type().empty()) {
        o.combo_type = utilities::from_string_combo(pb.combo_type());
    }
    return o;
}

std::string trade_serialize(const utilities::TradeData& t) {
    otrader::zmq::ZmqTradeData pb;
    pb.set_gateway_name(t.gateway_name);
    pb.set_symbol(t.symbol);
    pb.set_exchange(utilities::to_string(t.exchange));
    pb.set_orderid(t.orderid);
    pb.set_tradeid(t.tradeid);
    if (t.direction.has_value()) {
        pb.set_direction(utilities::to_string(*t.direction));
    }
    pb.set_price(t.price);
    pb.set_volume(t.volume);
    if (t.datetime.has_value()) {
        pb.set_datetime(utilities::datetime_to_iso_string(*t.datetime));
    }
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<utilities::TradeData> trade_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqTradeData pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    utilities::TradeData t;
    t.gateway_name = pb.gateway_name();
    t.symbol = pb.symbol();
    t.exchange = utilities::from_string_exchange(pb.exchange().empty() ? "LOCAL" : pb.exchange());
    t.orderid = pb.orderid();
    t.tradeid = pb.tradeid();
    if (!pb.direction().empty()) {
        t.direction = utilities::from_string_direction(pb.direction());
    }
    t.price = pb.price();
    t.volume = pb.volume();
    if (!pb.datetime().empty()) {
        t.datetime = utilities::datetime_from_iso_string(pb.datetime());
    }
    return t;
}

std::string order_request_serialize(const utilities::OrderRequest& r) {
    otrader::zmq::ZmqOrderRequest pb;
    pb.set_symbol(r.symbol);
    pb.set_exchange(utilities::to_string(r.exchange));
    pb.set_direction(utilities::to_string(r.direction));
    pb.set_type(utilities::to_string(r.type));
    pb.set_volume(r.volume);
    pb.set_price(r.price);
    pb.set_reference(r.reference);
    if (r.trading_class.has_value()) {
        pb.set_trading_class(*r.trading_class);
    }
    pb.set_is_combo(r.is_combo);
    if (r.legs.has_value()) {
        for (const auto& leg : *r.legs) {
            leg_to_proto(leg, pb.add_legs());
        }
    }
    if (r.combo_type.has_value()) {
        pb.set_combo_type(utilities::to_string(*r.combo_type));
    }
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<utilities::OrderRequest> order_request_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqOrderRequest pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    utilities::OrderRequest r;
    r.symbol = pb.symbol();
    r.exchange = utilities::from_string_exchange(pb.exchange().empty() ? "SMART" : pb.exchange());
    r.direction =
        utilities::from_string_direction(pb.direction().empty() ? "LONG" : pb.direction());
    r.type = utilities::from_string_ordertype(pb.type().empty() ? "LIMIT" : pb.type());
    r.volume = pb.volume();
    r.price = pb.price();
    r.reference = pb.reference();
    if (!pb.trading_class().empty()) {
        r.trading_class = pb.trading_class();
    }
    r.is_combo = pb.is_combo();
    for (int i = 0; i < pb.legs_size(); ++i) {
        if (!r.legs) {
            r.legs = std::vector<utilities::Leg>{};
        }
        r.legs->push_back(leg_from_proto(pb.legs(i)));
    }
    if (!pb.combo_type().empty()) {
        r.combo_type = utilities::from_string_combo(pb.combo_type());
    }
    return r;
}

std::string cancel_request_serialize(const utilities::CancelRequest& r) {
    otrader::zmq::ZmqCancelRequest pb;
    pb.set_orderid(r.orderid);
    pb.set_symbol(r.symbol);
    pb.set_exchange(utilities::to_string(r.exchange));
    pb.set_is_combo(r.is_combo);
    if (r.legs.has_value()) {
        for (const auto& leg : *r.legs) {
            leg_to_proto(leg, pb.add_legs());
        }
    }
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<utilities::CancelRequest> cancel_request_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqCancelRequest pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    utilities::CancelRequest r;
    r.orderid = pb.orderid();
    r.symbol = pb.symbol();
    r.exchange = utilities::from_string_exchange(pb.exchange().empty() ? "LOCAL" : pb.exchange());
    r.is_combo = pb.is_combo();
    for (int i = 0; i < pb.legs_size(); ++i) {
        if (!r.legs) {
            r.legs = std::vector<utilities::Leg>{};
        }
        r.legs->push_back(leg_from_proto(pb.legs(i)));
    }
    return r;
}

std::string connect_payload_serialize(const ZmqConnectPayload& p) {
    otrader::zmq::ZmqConnectPayload pb;
    pb.set_host(p.host);
    pb.set_port(p.port);
    pb.set_client_id(p.client_id);
    pb.set_account(p.account);
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<ZmqConnectPayload> connect_payload_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqConnectPayload pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    ZmqConnectPayload p;
    p.host = pb.host().empty() ? "127.0.0.1" : pb.host();
    p.port = pb.port() ? pb.port() : 7497;
    p.client_id = pb.client_id() ? pb.client_id() : 1;
    p.account = pb.account();
    return p;
}

std::string request_serialize(const std::string& cmd, const std::string& payload_bytes) {
    otrader::zmq::ZmqRequest pb;
    pb.set_cmd(cmd);
    pb.set_payload(payload_bytes);
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<ZmqRequest> request_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqRequest pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    ZmqRequest r;
    r.cmd = pb.cmd();
    r.payload = pb.payload();
    return r;
}

std::string response_serialize(const std::string& orderid) {
    otrader::zmq::ZmqResponse pb;
    pb.set_ok(true);
    pb.set_orderid(orderid);
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::string response_serialize_error(const std::string& err) {
    otrader::zmq::ZmqResponse pb;
    pb.set_ok(false);
    pb.set_error(err);
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::string response_serialize_ok() {
    otrader::zmq::ZmqResponse pb;
    pb.set_ok(true);
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<ZmqResponse> response_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqResponse pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    ZmqResponse r;
    r.ok = pb.ok();
    r.orderid = pb.orderid();
    r.error = pb.error();
    return r;
}

} // namespace engines
