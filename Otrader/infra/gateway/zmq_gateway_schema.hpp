#pragma once

/**
 * ZMQ Gateway message schema: Protobuf serialization for Order/Trade/OrderRequest/CancelRequest.
 * PUB/SUB topics: "order", "trade".
 * REQ/REP commands: connect, disconnect, send_order, cancel_order, query_account, query_position.
 */

#include "../../utilities/constant.hpp"
#include "../../utilities/object.hpp"
#include <optional>
#include <string>

namespace engines {

// Topic names for PUB/SUB
inline constexpr const char* ZMQ_TOPIC_ORDER = "order";
inline constexpr const char* ZMQ_TOPIC_TRADE = "trade";

// REQ/REP command keys
inline constexpr const char* ZMQ_CMD_CONNECT = "connect";
inline constexpr const char* ZMQ_CMD_DISCONNECT = "disconnect";
inline constexpr const char* ZMQ_CMD_SEND_ORDER = "send_order";
inline constexpr const char* ZMQ_CMD_CANCEL_ORDER = "cancel_order";
inline constexpr const char* ZMQ_CMD_QUERY_ACCOUNT = "query_account";
inline constexpr const char* ZMQ_CMD_QUERY_POSITION = "query_position";

/** Serialize OrderData to protobuf bytes. */
std::string order_serialize(const utilities::OrderData& o);

/** Deserialize OrderData from protobuf bytes. Returns nullopt on parse error. */
std::optional<utilities::OrderData> order_deserialize(const std::string& bytes);

/** Serialize TradeData to protobuf bytes. */
std::string trade_serialize(const utilities::TradeData& t);

/** Deserialize TradeData from protobuf bytes. */
std::optional<utilities::TradeData> trade_deserialize(const std::string& bytes);

/** Serialize OrderRequest to protobuf bytes (for send_order command). */
std::string order_request_serialize(const utilities::OrderRequest& r);

/** Deserialize OrderRequest from protobuf bytes. */
std::optional<utilities::OrderRequest> order_request_deserialize(const std::string& bytes);

/** Serialize CancelRequest to protobuf bytes. */
std::string cancel_request_serialize(const utilities::CancelRequest& r);

/** Deserialize CancelRequest from protobuf bytes. */
std::optional<utilities::CancelRequest> cancel_request_deserialize(const std::string& bytes);

/** Connect command payload (host, port, client_id, account). */
struct ZmqConnectPayload {
    std::string host = "127.0.0.1";
    int port = 7497;
    int client_id = 1;
    std::string account;
};
std::string connect_payload_serialize(const ZmqConnectPayload& p);
std::optional<ZmqConnectPayload> connect_payload_deserialize(const std::string& bytes);

/** Generic REQ/REP: request has cmd and payload (protobuf bytes). */
struct ZmqRequest {
    std::string cmd;
    std::string payload;
};
std::optional<ZmqRequest> request_deserialize(const std::string& bytes);
std::string request_serialize(const std::string& cmd, const std::string& payload_bytes);

std::string response_serialize(const std::string& orderid); // send_order success
std::string response_serialize_error(const std::string& err);
std::string response_serialize_ok(); // connect/disconnect/cancel/query success

/** Parsed response from REP. */
struct ZmqResponse {
    bool ok = false;
    std::string orderid;
    std::string error;
};
std::optional<ZmqResponse> response_deserialize(const std::string& bytes);

} // namespace engines
