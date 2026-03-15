#pragma once

/**
 * MarketDataClient: ZMQ REQ (commands) + SUB (Snapshot stream).
 * Connects to Market Data process; all market data ops go through ZMQ.
 */

#include "../../infra/gateway/zmq_gateway_schema.hpp"
#include "../../infra/marketdata/zmq_marketdata_schema.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace engines {

class MainEngine;

class MarketDataClient {
  public:
    explicit MarketDataClient(MainEngine* main_engine);
    ~MarketDataClient();

    void start();
    void stop();
    void subscribe_chains(const std::string& strategy_name,
                          std::span<const std::string> chain_symbols);
    void unsubscribe_chains(const std::string& strategy_name);

    bool running() const { return running_.load(); }

    void close();

  private:
    void run_sub_thread();
    std::optional<ZmqResponse> req_rep(const std::string& cmd, const std::string& payload);

    MainEngine* main_engine_ = nullptr;
    std::string rep_addr_;
    std::string pub_addr_;
    std::atomic<bool> running_{false};
    std::jthread sub_thread_;
};

} // namespace engines
