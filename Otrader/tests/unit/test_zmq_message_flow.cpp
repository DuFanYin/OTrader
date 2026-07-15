#include "infra/gateway/zmq_gateway_schema.hpp"
#include "infra/marketdata/zmq_marketdata_schema.hpp"
#include "utilities/object.hpp"
#include <gtest/gtest.h>

namespace {

TEST(ZmqMessageFlow, GatewayOrderAndTradeRoundTrip) {
    utilities::OrderData order;
    order.symbol = "SPXW 20260101C05000000";
    order.orderid = "ORDER-1001";
    order.exchange = utilities::Exchange::SMART;
    order.type = utilities::OrderType::LIMIT;
    order.direction = utilities::Direction::LONG;
    order.price = 1.25;
    order.volume = 3;
    order.status = utilities::Status::SUBMITTING;
    order.is_combo = true;
    order.combo_type = utilities::ComboType::STRADDLE;
    utilities::Leg leg1;
    leg1.con_id = 1001;
    leg1.exchange = utilities::Exchange::SMART;
    leg1.ratio = 1;
    leg1.direction = utilities::Direction::LONG;
    utilities::Leg leg2;
    leg2.con_id = 1002;
    leg2.exchange = utilities::Exchange::SMART;
    leg2.ratio = 1;
    leg2.direction = utilities::Direction::SHORT;
    order.legs = std::vector<utilities::Leg>{leg1, leg2};

    const std::string order_bytes = engines::order_serialize(order);
    ASSERT_FALSE(order_bytes.empty());
    const auto order_back = engines::order_deserialize(order_bytes);
    ASSERT_TRUE(order_back.has_value());
    EXPECT_EQ(order_back->orderid, order.orderid);
    EXPECT_EQ(order_back->symbol, order.symbol);
    ASSERT_TRUE(order_back->combo_type.has_value());
    EXPECT_EQ(*order_back->combo_type, utilities::ComboType::STRADDLE);
    ASSERT_TRUE(order_back->legs.has_value());
    EXPECT_EQ(order_back->legs->size(), 2u);

    utilities::TradeData trade;
    trade.symbol = order.symbol;
    trade.orderid = order.orderid;
    trade.tradeid = "TRADE-1";
    trade.exchange = utilities::Exchange::SMART;
    trade.direction = utilities::Direction::LONG;
    trade.price = 1.3;
    trade.volume = 1;
    const std::string trade_bytes = engines::trade_serialize(trade);
    ASSERT_FALSE(trade_bytes.empty());
    const auto trade_back = engines::trade_deserialize(trade_bytes);
    ASSERT_TRUE(trade_back.has_value());
    EXPECT_EQ(trade_back->tradeid, "TRADE-1");
    EXPECT_EQ(trade_back->orderid, order.orderid);
}

TEST(ZmqMessageFlow, GatewayRequestResponseRoundTrip) {
    engines::ZmqConnectPayload connect_payload;
    connect_payload.host = "127.0.0.1";
    connect_payload.port = 4001;
    connect_payload.client_id = 99;
    connect_payload.account = "DU123";

    const std::string payload = engines::connect_payload_serialize(connect_payload);
    ASSERT_FALSE(payload.empty());

    const std::string req_bytes = engines::request_serialize(engines::ZMQ_CMD_CONNECT, payload);
    const auto req = engines::request_deserialize(req_bytes);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->cmd, engines::ZMQ_CMD_CONNECT);

    const auto connect_back = engines::connect_payload_deserialize(req->payload);
    ASSERT_TRUE(connect_back.has_value());
    EXPECT_EQ(connect_back->port, 4001);
    EXPECT_EQ(connect_back->client_id, 99);
    EXPECT_EQ(connect_back->account, "DU123");

    const auto ok_rsp = engines::response_deserialize(engines::response_serialize_ok());
    ASSERT_TRUE(ok_rsp.has_value());
    EXPECT_TRUE(ok_rsp->ok);
    EXPECT_TRUE(ok_rsp->error.empty());

    const auto err_rsp = engines::response_deserialize(engines::response_serialize_error("boom"));
    ASSERT_TRUE(err_rsp.has_value());
    EXPECT_FALSE(err_rsp->ok);
    EXPECT_EQ(err_rsp->error, "boom");
}

TEST(ZmqMessageFlow, MarketSnapshotAndSubscribeRoundTrip) {
    utilities::PortfolioSnapshot snap;
    snap.portfolio_name = "SPXW";
    snap.underlying_bid = 5000.5;
    snap.underlying_ask = 5001.0;
    snap.bid = {1.0, 2.0};
    snap.ask = {1.1, 2.1};
    snap.last = {1.05, 2.05};
    snap.delta = {0.2, -0.2};
    snap.gamma = {0.01, 0.02};
    snap.theta = {-0.05, -0.07};
    snap.vega = {0.3, 0.4};
    snap.iv = {0.15, 0.2};

    const std::string snap_bytes = engines::snapshot_serialize(snap);
    ASSERT_FALSE(snap_bytes.empty());
    const auto snap_back = engines::snapshot_deserialize(snap_bytes);
    ASSERT_TRUE(snap_back.has_value());
    EXPECT_EQ(snap_back->portfolio_name, "SPXW");
    EXPECT_EQ(snap_back->bid.size(), 2u);
    EXPECT_DOUBLE_EQ(snap_back->underlying_ask, 5001.0);

    engines::ZmqSubscribeChainsPayload sub;
    sub.strategy_name = "straddle_spxw";
    sub.chain_symbols = {"SPXW-20260101", "SPXW-20260108"};
    const std::string sub_bytes = engines::subscribe_chains_payload_serialize(sub);
    const auto sub_back = engines::subscribe_chains_payload_deserialize(sub_bytes);
    ASSERT_TRUE(sub_back.has_value());
    EXPECT_EQ(sub_back->strategy_name, "straddle_spxw");
    ASSERT_EQ(sub_back->chain_symbols.size(), 2u);
    EXPECT_EQ(sub_back->chain_symbols[1], "SPXW-20260108");
}

} // namespace
