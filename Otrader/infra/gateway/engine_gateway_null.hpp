#pragma once

/**
 * NullGateway: no-op live gateway used when the engine is built without IB support
 * (BUILD_GATEWAY=OFF). Mirrors IbGateway's public surface so `LiveGateway` (see
 * live_gateway.hpp) can typedef to either without any other code change. Orders are
 * logged and rejected; nothing connects to a broker. Zero IBJts dependency.
 */

#include "../../core/engine_log.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include <functional>
#include <string>

namespace engines {

class NullGateway : public utilities::BaseEngine {
  public:
    explicit NullGateway(runtime_common::MainEngineBase* main_engine);

    void connect();
    void disconnect();
    std::string send_order(const utilities::OrderRequest& req);
    void cancel_order(const utilities::CancelRequest& req);
    void query_account();
    void query_position();

    void process_timer_event(const utilities::Event& event);

    void close() override { disconnect(); }

    const std::string& gateway_name() const { return engine_name; }
    bool is_connected() const { return false; }

    /** Same shape as IbGateway::Setting so callers compile unchanged. */
    struct Setting {
        std::string host = "127.0.0.1";
        int port = 7497;
        int client_id = 1;
        std::string account;
    };
    Setting& default_setting() { return default_setting_; }
    const Setting& default_setting() const { return default_setting_; }

    void set_order_callback(std::function<void(const utilities::OrderData&)> cb) {
        order_callback_ = std::move(cb);
    }
    void set_trade_callback(std::function<void(const utilities::TradeData&)> cb) {
        trade_callback_ = std::move(cb);
    }

  private:
    Setting default_setting_;
    std::function<void(const utilities::OrderData&)> order_callback_;
    std::function<void(const utilities::TradeData&)> trade_callback_;
};

} // namespace engines
