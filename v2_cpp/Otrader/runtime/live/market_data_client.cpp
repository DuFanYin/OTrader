/** MarketDataClient: ZMQ REQ + SUB to Market Data process. */

#include "market_data_client.hpp"
#include "engine_main.hpp"
#include <cstdlib>
#include <thread>
#include <zmq.hpp>

namespace engines {

MarketDataClient::MarketDataClient(MainEngine* main_engine) : main_engine_(main_engine) {
    const char* rep = std::getenv("MARKETDATA_REP_ADDR");
    const char* pub = std::getenv("MARKETDATA_PUB_ADDR");
    rep_addr_ = rep ? rep : "tcp://127.0.0.1:5557";
    pub_addr_ = pub ? pub : "tcp://127.0.0.1:5558";
}

MarketDataClient::~MarketDataClient() { close(); }

void MarketDataClient::run_sub_thread() {
    zmq::context_t ctx(1);
    zmq::socket_t sub(ctx, ZMQ_SUB);
    sub.connect(pub_addr_);
    sub.set(zmq::sockopt::subscribe, ZMQ_TOPIC_SNAPSHOT);
    sub.set(zmq::sockopt::rcvtimeo, sub_rcv_timeout_ms_);

    while (running_) {
        zmq::message_t topic_msg;
        auto rc = sub.recv(topic_msg, (sub_rcv_timeout_ms_ == 0) ? zmq::recv_flags::dontwait
                                                                 : zmq::recv_flags::none);
        if (!rc) {
            if (sub_rcv_timeout_ms_ == 0) {
                // Avoid busy-spin when running without blocking (test mode).
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            continue;
        }
        if (!running_) {
            break;
        }

        zmq::message_t payload_msg;
        rc = sub.recv(payload_msg, (sub_rcv_timeout_ms_ == 0) ? zmq::recv_flags::dontwait
                                                              : zmq::recv_flags::none);
        if (!rc) {
            break;
        }
        std::string payload(payload_msg.data<char>(), payload_msg.size());

        if (main_engine_ == nullptr) {
            continue;
        }

        auto s = snapshot_deserialize(payload);
        if (s) {
            auto* main = static_cast<MainEngine*>(main_engine_);
            utilities::PortfolioSnapshot* p = main ? main->acquire_snapshot() : nullptr;
            if (p != nullptr) {
                *p = std::move(*s);
                main_engine_->put_event(utilities::Event(utilities::EventType::Snapshot, p));
            }
        }
    }
}

std::optional<ZmqResponse> MarketDataClient::req_rep(const std::string& cmd,
                                                     const std::string& payload) {
    std::string req_bytes = request_serialize(cmd, payload);
    zmq::context_t ctx(1);
    zmq::socket_t req(ctx, ZMQ_REQ);
    req.connect(rep_addr_);
    req.set(zmq::sockopt::rcvtimeo, reqrep_timeout_ms_);
    req.set(zmq::sockopt::sndtimeo, reqrep_timeout_ms_);
    try {
        const auto send_flags =
            (reqrep_timeout_ms_ == 0) ? zmq::send_flags::dontwait : zmq::send_flags::none;
        req.send(zmq::buffer(req_bytes), send_flags);
    } catch (const zmq::error_t& e) {
        if (e.num() == EAGAIN) {
            return std::nullopt;
        }
        throw;
    }
    zmq::message_t reply;
    try {
        const auto recv_flags =
            (reqrep_timeout_ms_ == 0) ? zmq::recv_flags::dontwait : zmq::recv_flags::none;
        auto rc = req.recv(reply, recv_flags);
        if (!rc) {
            return std::nullopt;
        }
    } catch (const zmq::error_t& e) {
        if (e.num() == EAGAIN) {
            return std::nullopt;
        }
        throw;
    }
    return response_deserialize(std::string(reply.data<char>(), reply.size()));
}

void MarketDataClient::start() {
    running_ = true;
    sub_thread_ = std::jthread([this]() { run_sub_thread(); });
    // If timeout is 0 (test mode), guarantee start() never blocks on external processes.
    if (reqrep_timeout_ms_ > 0) {
        req_rep(ZMQ_CMD_START, std::string{});
    }
}

void MarketDataClient::stop(bool send_stop_cmd) {
    running_ = false;
    if (sub_thread_.joinable()) {
        try {
            // If timeout is 0 (test mode), guarantee stop() never blocks on external processes.
            if (send_stop_cmd && reqrep_timeout_ms_ > 0) {
                req_rep(ZMQ_CMD_STOP, std::string{});
            }
        } catch (...) {
            if (main_engine_ != nullptr) {
                main_engine_->write_log(
                    "MarketDataClient::stop: ZMQ teardown error (ignored so dtor does not throw)",
                    WARNING);
            }
        }
        sub_thread_.join();
    }
}

void MarketDataClient::subscribe_chains(const std::string& strategy_name,
                                        std::span<const std::string> chain_symbols) {
    ZmqSubscribeChainsPayload p;
    p.strategy_name = strategy_name;
    p.chain_symbols.assign(chain_symbols.begin(), chain_symbols.end());
    req_rep(ZMQ_CMD_SUBSCRIBE_CHAINS, subscribe_chains_payload_serialize(p));
}

void MarketDataClient::unsubscribe_chains(const std::string& strategy_name) {
    ZmqUnsubscribeChainsPayload p;
    p.strategy_name = strategy_name;
    req_rep(ZMQ_CMD_UNSUBSCRIBE_CHAINS, unsubscribe_chains_payload_serialize(p));
}

void MarketDataClient::close() { stop(/*send_stop_cmd*/ true); }

} // namespace engines
