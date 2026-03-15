/**
 * Gateway process: IbGateway + ZMQ REP (commands) + ZMQ PUB (Order/Trade).
 * Env: GATEWAY_REP_ADDR (default tcp://127.0.0.1:5555), GATEWAY_PUB_ADDR (default
 * tcp://127.0.0.1:5556).
 */

#include "infra/gateway/engine_gateway_ib.hpp"
#include "infra/gateway/zmq_gateway_schema.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <thread>
#include <zmq.hpp>

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running = false; }

int main() {
    std::signal(SIGINT, signal_handler);

    const char* rep_addr = std::getenv("GATEWAY_REP_ADDR");
    const char* pub_addr = std::getenv("GATEWAY_PUB_ADDR");
    if (rep_addr == nullptr)
        rep_addr = "tcp://127.0.0.1:5555";
    if (pub_addr == nullptr)
        pub_addr = "tcp://127.0.0.1:5556";

    zmq::context_t ctx(1);
    zmq::socket_t rep_socket(ctx, ZMQ_REP);
    zmq::socket_t pub_socket(ctx, ZMQ_PUB);
    rep_socket.bind(rep_addr);
    pub_socket.bind(pub_addr);

    std::cerr << std::format("[Gateway] REP bound to {} PUB bound to {}\n", rep_addr, pub_addr);

    engines::IbGateway gateway(nullptr);
    gateway.set_order_callback([&pub_socket](const utilities::OrderData& o) {
        std::string bytes = engines::order_serialize(o);
        zmq::message_t topic_msg(engines::ZMQ_TOPIC_ORDER, strlen(engines::ZMQ_TOPIC_ORDER));
        zmq::message_t payload_msg(bytes.data(), bytes.size());
        pub_socket.send(topic_msg, zmq::send_flags::sndmore);
        pub_socket.send(payload_msg, zmq::send_flags::none);
    });
    gateway.set_trade_callback([&pub_socket](const utilities::TradeData& t) {
        std::string bytes = engines::trade_serialize(t);
        zmq::message_t topic_msg(engines::ZMQ_TOPIC_TRADE, strlen(engines::ZMQ_TOPIC_TRADE));
        zmq::message_t payload_msg(bytes.data(), bytes.size());
        pub_socket.send(topic_msg, zmq::send_flags::sndmore);
        pub_socket.send(payload_msg, zmq::send_flags::none);
    });

    // Timer thread for TWS process_pending_messages
    std::jthread timer_thread([&gateway]() {
        while (g_running) {
            gateway.process_timer_event(utilities::Event(utilities::EventType::Timer));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    // REP loop
    while (g_running) {
        zmq::message_t req_msg;
        auto rc = rep_socket.recv(req_msg, zmq::recv_flags::dontwait);
        if (!rc) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        std::string req_bytes(req_msg.data<char>(), req_msg.size());
        auto req = engines::request_deserialize(req_bytes);
        if (!req) {
            rep_socket.send(zmq::buffer(engines::response_serialize_error("invalid request")),
                            zmq::send_flags::none);
            continue;
        }
        std::string resp;
        if (req->cmd == engines::ZMQ_CMD_CONNECT) {
            auto p = engines::connect_payload_deserialize(req->payload);
            if (p) {
                gateway.default_setting().host = p->host;
                gateway.default_setting().port = p->port;
                gateway.default_setting().client_id = p->client_id;
                gateway.default_setting().account = p->account;
                gateway.connect();
                resp = engines::response_serialize_ok();
            } else {
                resp = engines::response_serialize_error("invalid connect payload");
            }
        } else if (req->cmd == engines::ZMQ_CMD_DISCONNECT) {
            gateway.disconnect();
            resp = engines::response_serialize_ok();
        } else if (req->cmd == engines::ZMQ_CMD_SEND_ORDER) {
            auto r = engines::order_request_deserialize(req->payload);
            if (r) {
                std::string oid = gateway.send_order(*r);
                resp = engines::response_serialize(oid);
            } else {
                resp = engines::response_serialize_error("invalid order request");
            }
        } else if (req->cmd == engines::ZMQ_CMD_CANCEL_ORDER) {
            auto r = engines::cancel_request_deserialize(req->payload);
            if (r) {
                gateway.cancel_order(*r);
                resp = engines::response_serialize_ok();
            } else {
                resp = engines::response_serialize_error("invalid cancel request");
            }
        } else if (req->cmd == engines::ZMQ_CMD_QUERY_ACCOUNT) {
            gateway.query_account();
            resp = engines::response_serialize_ok();
        } else if (req->cmd == engines::ZMQ_CMD_QUERY_POSITION) {
            gateway.query_position();
            resp = engines::response_serialize_ok();
        } else {
            resp = engines::response_serialize_error("unknown command: " + req->cmd);
        }
        rep_socket.send(zmq::buffer(resp), zmq::send_flags::none);
    }

    gateway.close();
    std::cerr << "[Gateway] Shutdown\n";
    return 0;
}
