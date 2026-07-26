#pragma once

/**
 * GatewayClient: ZMQ REQ (commands) + SUB (Order/Trade stream).
 * Connects to Gateway process; all gateway ops go through ZMQ.
 */

#include "../../infra/gateway/zmq_gateway_schema.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <zmq.hpp>

namespace engines {

class MainEngine;

class GatewayClient {
  public:
    explicit GatewayClient(MainEngine* main_engine);
    ~GatewayClient();

    void connect();
    void disconnect();
    /** Start SUB thread (order/trade stream). Called by connect() on success; same pattern as
     * MarketDataClient::start(). */
    void start();
    std::string send_order(const utilities::OrderRequest& req);
    void cancel_order(const utilities::CancelRequest& req);
    void query_account();
    void query_position();

    /** True after connect() succeeds (Gateway process confirmed). */
    bool is_connected() const { return connected_.load(); }

    void close();

  private:
    void run_sub_thread();
    std::optional<ZmqResponse> req_rep(const std::string& cmd, const std::string& payload);

    MainEngine* main_engine_ = nullptr;
    std::string rep_addr_;
    std::string pub_addr_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::jthread sub_thread_;

    // Persistent REQ command channel. ZMQ sockets are not thread-safe and REQ/REP is strict
    // lock-step, so req_rep() serializes each roundtrip under req_mutex_ instead of building a
    // fresh context+socket per call (the old approach cost ~ms/order; see latencyFindings F-3).
    // Lazily created on first use so tests that never call gateway ops pay nothing.
    std::mutex req_mutex_;
    std::unique_ptr<zmq::context_t> req_ctx_;
    std::unique_ptr<zmq::socket_t> req_sock_;
};

} // namespace engines
