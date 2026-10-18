#pragma once

/**
 * Orders, fills, account and contract sync through IB Gateway / TWS, on its own connection
 * (IB_CLIENT_ID); quotes use another.
 */

#include "utilities/log_level.hpp"
#include "model/object.hpp"
#include "adapters/ib_api/ib_api.hpp"
#include "core/gateway/gateway.hpp"
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adapters {

class IbTws;

class IbGateway : public core::Gateway {
  public:
    explicit IbGateway(utilities::Logger* logger);
    ~IbGateway() override;

    IbGateway(const IbGateway&) = delete;
    IbGateway& operator=(const IbGateway&) = delete;

    /** IbSettings::from_env() at construction; change before connect(). */
    IbSettings& default_setting() { return setting_; }
    const IbSettings& default_setting() const { return setting_; }

    /** Connect to default_setting(); throws when IB cannot be reached. */
    void connect() override;
    void disconnect() override;
    std::string send_order(const model::OrderRequest& req) override;
    void cancel_order(const model::CancelRequest& req) override;
    /** The account's values, and its positions with what each is worth, once connected; the
     * future fails if IB does not answer within a minute. */
    std::future<core::BrokerAccount> query_account() override;

    /** Underlying, hedge and options expiring within max_dte New York days. Throws for an unknown
     * portfolio or listing, or when not connected. Times out after five minutes (IB's first query
     * after login can be that slow). One sync at a time. */
    std::future<std::vector<model::Contract>> sync_contracts(std::string_view portfolio,
                                                             int max_dte) override;

    /** Before connect(): keeps updates flowing to restored working orders, never reuses ids. */
    void restore_orders(std::span<const model::Order> orders) override;
    /** After connect(): replays today's fills (engine dedups) and finishes restored orders that
     * ended while down. One-minute timeout. */
    std::future<void> catch_up() override;

    bool has_broker() const override { return true; }
    bool is_connected() const override;

  private:
    IbSettings setting_;
    std::unique_ptr<IbTws> tws_; // the connection (ib_gateway.cpp)
};

} // namespace adapters
