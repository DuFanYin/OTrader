/** ZMQ Market Data schema: Protobuf serialization. */

#include "zmq_marketdata_schema.hpp"
#include "../../proto/zmq_messages.pb.h"
#include "../../utilities/datetime_serialize.hpp"

namespace engines {

std::string snapshot_serialize(const utilities::PortfolioSnapshot& s) {
    otrader::zmq::ZmqPortfolioSnapshot pb;
    pb.set_portfolio_name(s.portfolio_name);
    pb.set_datetime(utilities::datetime_to_iso_string(s.datetime));
    pb.set_underlying_bid(s.underlying_bid);
    pb.set_underlying_ask(s.underlying_ask);
    pb.set_underlying_last(s.underlying_last);
    for (double v : s.bid) {
        pb.add_bid(v);
    }
    for (double v : s.ask) {
        pb.add_ask(v);
    }
    for (double v : s.last) {
        pb.add_last(v);
    }
    for (double v : s.delta) {
        pb.add_delta(v);
    }
    for (double v : s.gamma) {
        pb.add_gamma(v);
    }
    for (double v : s.theta) {
        pb.add_theta(v);
    }
    for (double v : s.vega) {
        pb.add_vega(v);
    }
    for (double v : s.iv) {
        pb.add_iv(v);
    }
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<utilities::PortfolioSnapshot> snapshot_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqPortfolioSnapshot pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    utilities::PortfolioSnapshot s;
    s.portfolio_name = pb.portfolio_name();
    if (auto dt = utilities::datetime_from_iso_string(pb.datetime())) {
        s.datetime = *dt;
    }
    s.underlying_bid = pb.underlying_bid();
    s.underlying_ask = pb.underlying_ask();
    s.underlying_last = pb.underlying_last();
    for (int i = 0; i < pb.bid_size(); ++i) {
        s.bid.push_back(pb.bid(i));
    }
    for (int i = 0; i < pb.ask_size(); ++i) {
        s.ask.push_back(pb.ask(i));
    }
    for (int i = 0; i < pb.last_size(); ++i) {
        s.last.push_back(pb.last(i));
    }
    for (int i = 0; i < pb.delta_size(); ++i) {
        s.delta.push_back(pb.delta(i));
    }
    for (int i = 0; i < pb.gamma_size(); ++i) {
        s.gamma.push_back(pb.gamma(i));
    }
    for (int i = 0; i < pb.theta_size(); ++i) {
        s.theta.push_back(pb.theta(i));
    }
    for (int i = 0; i < pb.vega_size(); ++i) {
        s.vega.push_back(pb.vega(i));
    }
    for (int i = 0; i < pb.iv_size(); ++i) {
        s.iv.push_back(pb.iv(i));
    }
    return s;
}

std::string subscribe_chains_payload_serialize(const ZmqSubscribeChainsPayload& p) {
    otrader::zmq::ZmqSubscribeChainsPayload pb;
    pb.set_strategy_name(p.strategy_name);
    for (const auto& sym : p.chain_symbols) {
        pb.add_chain_symbols(sym);
    }
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<ZmqSubscribeChainsPayload>
subscribe_chains_payload_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqSubscribeChainsPayload pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    ZmqSubscribeChainsPayload p;
    p.strategy_name = pb.strategy_name();
    for (int i = 0; i < pb.chain_symbols_size(); ++i) {
        p.chain_symbols.push_back(pb.chain_symbols(i));
    }
    return p;
}

std::string unsubscribe_chains_payload_serialize(const ZmqUnsubscribeChainsPayload& p) {
    otrader::zmq::ZmqUnsubscribeChainsPayload pb;
    pb.set_strategy_name(p.strategy_name);
    std::string out;
    return pb.SerializeToString(&out) ? out : "";
}

std::optional<ZmqUnsubscribeChainsPayload>
unsubscribe_chains_payload_deserialize(const std::string& bytes) {
    otrader::zmq::ZmqUnsubscribeChainsPayload pb;
    if (!pb.ParseFromString(bytes)) {
        return std::nullopt;
    }
    ZmqUnsubscribeChainsPayload p;
    p.strategy_name = pb.strategy_name();
    return p;
}

} // namespace engines
