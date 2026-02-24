#pragma once

/**
 * C++ equivalent of engines/engine_gateway.py (IbGateway).
 * Interface: connect, disconnect, send_order, cancel_order, query_account, query_position, query_portfolio.
 * Actual IB connectivity via IbApi implementation (stub or TWS).
 */

#include "../../utilities/object.hpp"
#include "../../utilities/event.hpp"
#include "../../core/engine_log.hpp"
#include <memory>
#include <string>

namespace engines {

struct MainEngine;
class IbApiTws;  // TWS implementation in engine_gateway.cpp; needs to call gateway private callbacks

/** Abstract API for IB connectivity (stub or real TWS). */
class IbApi {
public:
    virtual ~IbApi() = default;
    virtual bool is_connected() const { return false; }
    virtual void connect(const std::string& host, int port, int client_id, const std::string& account) = 0;
    virtual void close() = 0;
    virtual void check_connection() = 0;
    virtual std::string send_order(const utilities::OrderRequest& req) = 0;
    virtual void cancel_order(const utilities::CancelRequest& req) = 0;
    virtual void query_account() = 0;
    virtual void query_position() = 0;
    virtual void query_portfolio(const std::string& underlying) = 0;
    /** Call periodically when connected to drain TWS message queue (no-op for stub). */
    virtual void process_pending_messages() {}
};

class IbGateway {
    friend class IbApi;
    friend class IbApiTws;
public:
    explicit IbGateway(MainEngine* main_engine);
    ~IbGateway();

    void connect();
    void disconnect();
    std::string send_order(const utilities::OrderRequest& req);
    void cancel_order(const utilities::CancelRequest& req);
    void query_account();
    void query_position();
    void query_portfolio(const std::string& underlying);

    void process_timer_event(const utilities::Event& event);

    const std::string& gateway_name() const { return gateway_name_; }
    /** 是否已与 IB/TWS 建立连接。 */
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

private:
    void write_log(const std::string& msg, int level = engines::INFO);
    void on_order(const utilities::OrderData& order);
    void on_trade(const utilities::TradeData& trade);
    void on_contract(const utilities::ContractData& contract);

    MainEngine* main_engine_ = nullptr;
    std::string gateway_name_ = "IBGateway";
    Setting default_setting_;
    int     count_ = 0;
    std::unique_ptr<IbApi> api_;
};

}  // namespace engines
