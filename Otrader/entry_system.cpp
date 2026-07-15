/**
 * entry_system.cpp
 *
 * Unified entry for live system with multiple modes:
 *
 *   entry_system --mode=gateway       # ZMQ REP+PUB + IbGateway
 *   entry_system --mode=market        # ZMQ REP+PUB + MarketDataEngine
 *   entry_system --mode=live          # gRPC + MainEngine runtime
 *   entry_system --mode=all           # start gateway + market as subprocesses, then live
 *
 * This file is a thin wrapper that routes to the existing process-level logic for
 * entry_gateway.cpp, entry_market_data.cpp, and entry_live_grpc.cpp.
 */

#include "infra/db/engine_db_pg.hpp"
#include "infra/gateway/engine_gateway_ib.hpp"
#include "infra/gateway/zmq_gateway_schema.hpp"
#include "infra/marketdata/engine_data_tradier.hpp"
#include "infra/marketdata/zmq_marketdata_schema.hpp"
#include "runtime/live/engine_grpc.hpp"
#include "runtime/live/engine_main.hpp"

#include "utilities/event.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <zmq.hpp>

namespace {

// Shared helpers
using namespace std::chrono_literals;

// ---------------- Gateway mode ----------------

std::atomic<bool> g_gateway_running{true};

void gateway_signal_handler(int) { g_gateway_running = false; }

int run_gateway() {
    std::signal(SIGINT, gateway_signal_handler);

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
        while (g_gateway_running) {
            gateway.process_timer_event(utilities::Event(utilities::EventType::Timer));
            std::this_thread::sleep_for(200ms);
        }
    });

    // REP loop
    while (g_gateway_running) {
        zmq::message_t req_msg;
        auto rc = rep_socket.recv(req_msg, zmq::recv_flags::dontwait);
        if (!rc) {
            std::this_thread::sleep_for(10ms);
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

// ---------------- Market Data mode ----------------

std::atomic<bool> g_market_running{true};

void market_signal_handler(int) { g_market_running = false; }

int run_market_data() {
    std::signal(SIGINT, market_signal_handler);

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
    while (g_market_running) {
        zmq::message_t req_msg;
        auto rc = rep_socket.recv(req_msg, zmq::recv_flags::dontwait);
        if (!rc) {
            std::this_thread::sleep_for(10ms);
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

// ---------------- Live (gRPC + MainEngine) mode ----------------

int run_live_grpc() {
    engines::MainEngine main_engine;

    // Build gRPC service, holds MainEngine*
    engines::GrpcLiveEngineService service(&main_engine);

    // Start gRPC server
    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::fprintf(stderr, "Failed to start gRPC server on 0.0.0.0:50051\n");
        return 1;
    }

    std::printf("Live gRPC engine listening on 0.0.0.0:50051\n");
    // Block until external signal
    server->Wait();

    // Close engine before exit
    try {
        main_engine.disconnect();
        main_engine.close();
    } catch (...) {
        // Fail quietly, avoid exception from main
    }

    return 0;
}

// ---------------- System-all mode ----------------

int run_system_all() {
    // Start gateway and market as background subprocesses using the same binary.
    int r1 = std::system("entry_system --mode=gateway &");
    if (r1 != 0) {
        std::fprintf(stderr, "[System] Failed to start gateway subprocess (code=%d)\n", r1);
        return 1;
    }

    int r2 = std::system("entry_system --mode=market &");
    if (r2 != 0) {
        std::fprintf(stderr, "[System] Failed to start market_data subprocess (code=%d)\n", r2);
        return 1;
    }

    // Current process runs live gRPC in foreground.
    return run_live_grpc();
}

} // namespace

int main(int argc, char* argv[]) {
    std::string mode = "live"; // 默认行为：与原 entry_live_grpc 等价
    if (argc >= 2) {
        std::string_view arg{argv[1]};
        if (arg.rfind("--mode=", 0) == 0) {
            mode = std::string(arg.substr(7));
        }
    }

    if (mode == "gateway")
        return run_gateway();
    if (mode == "market")
        return run_market_data();
    if (mode == "live")
        return run_live_grpc();
    if (mode == "all")
        return run_system_all();

    std::fprintf(stderr,
                 "Usage: entry_system --mode=gateway|market|live|all\n"
                 "  gateway : run ZMQ + IB gateway process\n"
                 "  market  : run market-data provider process\n"
                 "  live    : run gRPC + MainEngine runtime (default)\n"
                 "  all     : start gateway + market as subprocesses, then live in foreground\n");
    return 1;
}
