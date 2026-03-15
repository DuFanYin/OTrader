/**
 * Market Data process: MarketDataEngine + ZMQ REP (commands) + ZMQ PUB (Snapshot).
 * Env: MARKETDATA_REP_ADDR (default tcp://127.0.0.1:5557), MARKETDATA_PUB_ADDR (default
 * tcp://127.0.0.1:5558). Requires: DATABASE_URL, TRADIER_TOKEN.
 */

#include "infra/db/engine_db_pg.hpp"
#include "infra/gateway/zmq_gateway_schema.hpp"
#include "infra/marketdata/engine_data_tradier.hpp"
#include "infra/marketdata/zmq_marketdata_schema.hpp"
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

    const char* rep_addr = std::getenv("MARKETDATA_REP_ADDR");
    const char* pub_addr = std::getenv("MARKETDATA_PUB_ADDR");
    if (rep_addr == nullptr)
        rep_addr = "tcp://127.0.0.1:5557";
    if (pub_addr == nullptr)
        pub_addr = "tcp://127.0.0.1:5558";

    zmq::context_t ctx(1);
    zmq::socket_t rep_socket(ctx, ZMQ_REP);
    zmq::socket_t pub_socket(ctx, ZMQ_PUB);
    rep_socket.bind(rep_addr);
    pub_socket.bind(pub_addr);

    std::cerr << std::format("[MarketData] REP bound to {} PUB bound to {}\n", rep_addr, pub_addr);

    engines::DatabaseEngine db_engine(nullptr);
    engines::MarketDataEngine market_data(nullptr);
    market_data.set_snapshot_callback([&pub_socket](const utilities::PortfolioSnapshot& s) {
        std::string bytes = engines::snapshot_serialize(s);
        zmq::message_t topic_msg(engines::ZMQ_TOPIC_SNAPSHOT, strlen(engines::ZMQ_TOPIC_SNAPSHOT));
        zmq::message_t payload_msg(bytes.data(), bytes.size());
        pub_socket.send(topic_msg, zmq::send_flags::sndmore);
        pub_socket.send(payload_msg, zmq::send_flags::none);
    });

    market_data.ensure_portfolios_created();
    db_engine.load_contracts(
        [&market_data](const utilities::ContractData& c) { market_data.process_option(c); },
        [&market_data](const utilities::ContractData& c) { market_data.process_underlying(c); });
    market_data.finalize_all_chains();

    // REP loop (request/response envelope same as gateway: ZmqRequest / ZmqResponse)
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
        if (req->cmd == engines::ZMQ_CMD_START) {
            market_data.start_market_data_update();
            resp = engines::response_serialize_ok();
        } else if (req->cmd == engines::ZMQ_CMD_STOP) {
            market_data.stop_market_data_update();
            resp = engines::response_serialize_ok();
        } else if (req->cmd == engines::ZMQ_CMD_SUBSCRIBE_CHAINS) {
            auto p = engines::subscribe_chains_payload_deserialize(req->payload);
            if (p) {
                market_data.subscribe_chains(p->strategy_name, p->chain_symbols);
                resp = engines::response_serialize_ok();
            } else {
                resp = engines::response_serialize_error("invalid subscribe payload");
            }
        } else if (req->cmd == engines::ZMQ_CMD_UNSUBSCRIBE_CHAINS) {
            auto p = engines::unsubscribe_chains_payload_deserialize(req->payload);
            if (p) {
                market_data.unsubscribe_chains(p->strategy_name);
                resp = engines::response_serialize_ok();
            } else {
                resp = engines::response_serialize_error("invalid unsubscribe payload");
            }
        } else {
            resp = engines::response_serialize_error("unknown command: " + req->cmd);
        }
        rep_socket.send(zmq::buffer(resp), zmq::send_flags::none);
    }

    market_data.stop_market_data_update();
    db_engine.close();
    std::cerr << "[MarketData] Shutdown\n";
    return 0;
}
