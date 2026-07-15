#include "infra/gateway/zmq_gateway_schema.hpp"
#include "infra/marketdata/zmq_marketdata_schema.hpp"
#include "utilities/object.hpp"
#include <gtest/gtest.h>
#include <string>

namespace {

TEST(ZmqSchemaNegative, GatewayOrderDeserializeRejectsGarbage) {
    EXPECT_FALSE(engines::order_deserialize(std::string{}).has_value());
    EXPECT_FALSE(engines::order_deserialize("not protobuf").has_value());
    EXPECT_FALSE(engines::order_deserialize(std::string(3, '\0')).has_value());
}

TEST(ZmqSchemaNegative, GatewayTradeDeserializeRejectsGarbage) {
    EXPECT_FALSE(engines::trade_deserialize(std::string{}).has_value());
    EXPECT_FALSE(engines::trade_deserialize("not protobuf").has_value());
}

TEST(ZmqSchemaNegative, GatewayRequestResponseRejectsGarbage) {
    EXPECT_FALSE(engines::request_deserialize(std::string{}).has_value());
    EXPECT_FALSE(engines::request_deserialize("nope").has_value());

    EXPECT_FALSE(engines::response_deserialize(std::string{}).has_value());
    EXPECT_FALSE(engines::response_deserialize("nope").has_value());
}

TEST(ZmqSchemaNegative, GatewayConnectPayloadRejectsGarbage) {
    EXPECT_FALSE(engines::connect_payload_deserialize(std::string{}).has_value());
    EXPECT_FALSE(engines::connect_payload_deserialize("nope").has_value());
}

TEST(ZmqSchemaNegative, MarketSnapshotRejectsGarbage) {
    EXPECT_FALSE(engines::snapshot_deserialize(std::string{}).has_value());
    EXPECT_FALSE(engines::snapshot_deserialize("nope").has_value());
}

TEST(ZmqSchemaNegative, MarketSubscribeUnsubscribeRejectsGarbage) {
    EXPECT_FALSE(engines::subscribe_chains_payload_deserialize(std::string{}).has_value());
    EXPECT_FALSE(engines::subscribe_chains_payload_deserialize("nope").has_value());
    EXPECT_FALSE(engines::unsubscribe_chains_payload_deserialize(std::string{}).has_value());
    EXPECT_FALSE(engines::unsubscribe_chains_payload_deserialize("nope").has_value());
}

} // namespace
