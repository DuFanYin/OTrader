#pragma once

/**
 * ZMQ Market Data schema: Protobuf serialization for PortfolioSnapshot + commands.
 * PUB topic: "snapshot".
 * REQ/REP commands: start, stop, subscribe_chains, unsubscribe_chains.
 * REQ/REP envelope uses same ZmqRequest/ZmqResponse as gateway (see zmq_gateway_schema).
 */

#include "../../utilities/object.hpp"
#include <optional>
#include <string>
#include <vector>

namespace engines {

inline constexpr const char* ZMQ_TOPIC_SNAPSHOT = "snapshot";

inline constexpr const char* ZMQ_CMD_START = "start";
inline constexpr const char* ZMQ_CMD_STOP = "stop";
inline constexpr const char* ZMQ_CMD_SUBSCRIBE_CHAINS = "subscribe_chains";
inline constexpr const char* ZMQ_CMD_UNSUBSCRIBE_CHAINS = "unsubscribe_chains";

std::string snapshot_serialize(const utilities::PortfolioSnapshot& s);
std::optional<utilities::PortfolioSnapshot> snapshot_deserialize(const std::string& bytes);

/** subscribe_chains payload */
struct ZmqSubscribeChainsPayload {
    std::string strategy_name;
    std::vector<std::string> chain_symbols;
};
std::string subscribe_chains_payload_serialize(const ZmqSubscribeChainsPayload& p);
std::optional<ZmqSubscribeChainsPayload>
subscribe_chains_payload_deserialize(const std::string& bytes);

/** unsubscribe_chains payload */
struct ZmqUnsubscribeChainsPayload {
    std::string strategy_name;
};
std::string unsubscribe_chains_payload_serialize(const ZmqUnsubscribeChainsPayload& p);
std::optional<ZmqUnsubscribeChainsPayload>
unsubscribe_chains_payload_deserialize(const std::string& bytes);

} // namespace engines
