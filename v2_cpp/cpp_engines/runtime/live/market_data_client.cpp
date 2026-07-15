/** MarketDataClient: ZMQ REQ + SUB to Market Data process. */

#include "market_data_client.hpp"
#include "engine_main.hpp"
#include <cstdlib>
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
    sub.set(zmq::sockopt::rcvtimeo, 500);

    while (running_) {
        zmq::message_t topic_msg;
        auto rc = sub.recv(topic_msg, zmq::recv_flags::none);
        if (!rc) {
            continue;
        }
        if (!running_) {
            break;
        }

        zmq::message_t payload_msg;
        rc = sub.recv(payload_msg, zmq::recv_flags::none);
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
    req.set(zmq::sockopt::rcvtimeo, 5000);
    req.set(zmq::sockopt::sndtimeo, 5000);
    req.send(zmq::buffer(req_bytes), zmq::send_flags::none);
    zmq::message_t reply;
    auto rc = req.recv(reply, zmq::recv_flags::none);
    if (!rc) {
        return std::nullopt;
    }
    return response_deserialize(std::string(reply.data<char>(), reply.size()));
}

void MarketDataClient::start() {
    running_ = true;
    sub_thread_ = std::jthread([this]() { run_sub_thread(); });
    req_rep(ZMQ_CMD_START, std::string{});
}

void MarketDataClient::stop() {
    try {
        req_rep(ZMQ_CMD_STOP, std::string{});
    } catch (...) {
        if (main_engine_ != nullptr) {
            main_engine_->write_log(
                "MarketDataClient::stop: ZMQ teardown error (ignored so dtor does not throw)",
                WARNING);
        }
    }
    running_ = false;
    if (sub_thread_.joinable()) {
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

void MarketDataClient::close() { stop(); }

} // namespace engines
