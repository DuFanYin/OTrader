#pragma once

/**
 * C++ equivalent of engines/engine_gateway.py (IbGateway).
 * Interface: connect, disconnect, send_order, cancel_order, query_account, query_position.
 * Actual IB connectivity via IbApi implementation (stub or TWS).
 */

#include "../../core/engine_log.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include <functional>
#include <memory>
#include <string>

namespace engines {

class IbApiTws; // TWS implementation in engine_gateway.cpp

/** Abstract API for IB connectivity (stub or real TWS). */
class IbApi {
  public:
    virtual ~IbApi() = default;
    virtual bool is_connected() const { return false; }
    virtual void connect(const std::string& host, int port, int client_id,
                         const std::string& account) = 0;
    virtual void close() = 0;
    virtual void check_connection() = 0;
    virtual std::string send_order(const utilities::OrderRequest& req) = 0;
    virtual void cancel_order(const utilities::CancelRequest& req) = 0;
    virtual void query_account() = 0;
    virtual void query_position() = 0;
    /** Call periodically when connected to drain TWS message queue (no-op for stub). */
    virtual void process_pending_messages() {}
};

class IbGateway : public utilities::BaseEngine {
    friend class IbApi;
    friend class IbApiTws;

  public:
    explicit IbGateway(utilities::MainEngine* main_engine);
    ~IbGateway();

    void connect();
    void disconnect();
    std::string send_order(const utilities::OrderRequest& req);
    void cancel_order(const utilities::CancelRequest& req);
    void query_account();
    void query_position();

    void process_timer_event(const utilities::Event& event);

    void close() override { disconnect(); }

    const std::string& gateway_name() const { return engine_name; }
    /** Whether connected to IB/TWS. */
    bool is_connected() const;

    /** Python default_setting equivalent. */
    struct Setting {
        std::string host = "127.0.0.1";
        int port = 7497;
        int client_id = 1;
        std::string account;
    };
    Setting& default_setting() { return default_setting_; }
    const Setting& default_setting() const { return default_setting_; }

    /** Optional callbacks for ZMQ Gateway process: when set, used instead of
     * main_engine->put_event. */
    void set_order_callback(std::function<void(const utilities::OrderData&)> cb) {
        order_callback_ = std::move(cb);
    }
    void set_trade_callback(std::function<void(const utilities::TradeData&)> cb) {
        trade_callback_ = std::move(cb);
    }

  private:
    void on_order(const utilities::OrderData& order);
    void on_trade(const utilities::TradeData& trade);

    Setting default_setting_;
    std::function<void(const utilities::OrderData&)> order_callback_;
    std::function<void(const utilities::TradeData&)> trade_callback_;
    int count_ = 0;
    std::unique_ptr<IbApi> api_;
};

} // namespace engines
