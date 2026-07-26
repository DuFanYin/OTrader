/** GatewayClient: ZMQ REQ + SUB to Gateway process. */

#include "gateway_client.hpp"
#include "engine_main.hpp"
#include <cstdlib>
#include <zmq.hpp>

namespace engines {

GatewayClient::GatewayClient(MainEngine* main_engine) : main_engine_(main_engine) {
    const char* rep = std::getenv("GATEWAY_REP_ADDR");
    const char* pub = std::getenv("GATEWAY_PUB_ADDR");
    rep_addr_ = rep ? rep : "tcp://127.0.0.1:5555";
    pub_addr_ = pub ? pub : "tcp://127.0.0.1:5556";
}

GatewayClient::~GatewayClient() { close(); }

void GatewayClient::run_sub_thread() {
    zmq::context_t ctx(1);
    zmq::socket_t sub(ctx, ZMQ_SUB);
    sub.connect(pub_addr_);
    sub.set(zmq::sockopt::subscribe, ZMQ_TOPIC_ORDER);
    sub.set(zmq::sockopt::subscribe, ZMQ_TOPIC_TRADE);
    sub.set(zmq::sockopt::rcvtimeo, 500);

    while (running_) {
        zmq::message_t topic_msg;
        auto rc = sub.recv(topic_msg, zmq::recv_flags::none);
        if (!rc) {
            continue; // timeout or error
        }
        if (!running_) {
            break;
        }
        std::string topic(topic_msg.data<char>(), topic_msg.size());

        zmq::message_t payload_msg;
        rc = sub.recv(payload_msg, zmq::recv_flags::none);
        if (!rc) {
            break;
        }
        std::string payload(payload_msg.data<char>(), payload_msg.size());

        if (main_engine_ == nullptr) {
            continue;
        }

        if (topic == ZMQ_TOPIC_ORDER) {
            auto o = order_deserialize(payload);
            if (o) {
                auto* main = static_cast<MainEngine*>(main_engine_);
                utilities::OrderData* p = main ? main->acquire_order() : nullptr;
                if (p != nullptr) {
                    *p = std::move(*o);
                    main_engine_->put_event(utilities::Event(utilities::EventType::Order, p));
                }
            }
        } else if (topic == ZMQ_TOPIC_TRADE) {
            auto t = trade_deserialize(payload);
            if (t) {
                auto* main = static_cast<MainEngine*>(main_engine_);
                utilities::TradeData* p = main ? main->acquire_trade() : nullptr;
                if (p != nullptr) {
                    *p = std::move(*t);
                    main_engine_->put_event(utilities::Event(utilities::EventType::Trade, p));
                }
            }
        }
    }
}

std::optional<ZmqResponse> GatewayClient::req_rep(const std::string& cmd,
                                                  const std::string& payload) {
    std::string req_bytes = request_serialize(cmd, payload);

    // Serialize the whole roundtrip: one persistent REQ socket, strict send→recv lock-step,
    // shared by strategy/main/gRPC threads. Building the context+socket once (not per call)
    // removes the per-order ZMQ IO-thread + connect cost (latencyFindings F-3).
    std::lock_guard<std::mutex> lock(req_mutex_);

    auto make_socket = [this]() {
        if (!req_ctx_) {
            req_ctx_ = std::make_unique<zmq::context_t>(1);
        }
        req_sock_ = std::make_unique<zmq::socket_t>(*req_ctx_, ZMQ_REQ);
        req_sock_->connect(rep_addr_);
        req_sock_->set(zmq::sockopt::rcvtimeo, 5000);
        req_sock_->set(zmq::sockopt::sndtimeo, 5000);
        // Drop pending messages immediately on close so a dead socket can't wedge shutdown.
        req_sock_->set(zmq::sockopt::linger, 0);
    };

    if (!req_sock_) {
        make_socket();
    }

    try {
        req_sock_->send(zmq::buffer(req_bytes), zmq::send_flags::none);
        zmq::message_t reply;
        auto rc = req_sock_->recv(reply, zmq::recv_flags::none);
        if (!rc) {
            // Timeout: a REQ socket is now stuck in the recv state and unusable for the next
            // send. Recreate it so subsequent calls aren't permanently wedged.
            req_sock_.reset();
            return std::nullopt;
        }
        return response_deserialize(std::string(reply.data<char>(), reply.size()));
    } catch (const zmq::error_t&) {
        req_sock_.reset(); // reset the REQ state machine on any ZMQ error
        return std::nullopt;
    }
}

void GatewayClient::start() {
    if (running_.exchange(true)) {
        return;
    }
    sub_thread_ = std::jthread([this]() { run_sub_thread(); });
}

void GatewayClient::connect() {
    ZmqConnectPayload p;
    p.host = "127.0.0.1";
    p.port = 7497;
    p.client_id = 1;
    const char* host = std::getenv("IB_HOST");
    const char* port_s = std::getenv("IB_PORT");
    const char* client_s = std::getenv("IB_CLIENT_ID");
    const char* account = std::getenv("IB_ACCOUNT");
    if (host) {
        p.host = host;
    }
    if (port_s) {
        p.port = std::atoi(port_s);
    }
    if (client_s) {
        p.client_id = std::atoi(client_s);
    }
    if (account) {
        p.account = account;
    }

    auto resp = req_rep(ZMQ_CMD_CONNECT, connect_payload_serialize(p));
    connected_ = resp && resp->ok;
    if (connected_) {
        start();
    }
}

void GatewayClient::disconnect() {
    req_rep(ZMQ_CMD_DISCONNECT, std::string{});
    connected_ = false;
}

std::string GatewayClient::send_order(const utilities::OrderRequest& req) {
    auto resp = req_rep(ZMQ_CMD_SEND_ORDER, order_request_serialize(req));
    return (resp && resp->ok) ? resp->orderid : "";
}

void GatewayClient::cancel_order(const utilities::CancelRequest& req) {
    req_rep(ZMQ_CMD_CANCEL_ORDER, cancel_request_serialize(req));
}

void GatewayClient::query_account() { req_rep(ZMQ_CMD_QUERY_ACCOUNT, std::string{}); }

void GatewayClient::query_position() { req_rep(ZMQ_CMD_QUERY_POSITION, std::string{}); }

void GatewayClient::close() {
    running_ = false;
    if (sub_thread_.joinable()) {
        sub_thread_.join();
    }
    connected_ = false;
    // Tear down the persistent REQ channel (linger=0 → no wedged shutdown).
    std::lock_guard<std::mutex> lock(req_mutex_);
    req_sock_.reset();
    req_ctx_.reset();
}

} // namespace engines
