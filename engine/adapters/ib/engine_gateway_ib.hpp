#pragma once

/**
 * IbGateway: orders, fills, account and positions through IB Gateway / TWS (the TWS C++ API), on
 * a connection of its own as IB_CLIENT_ID; quotes come from the market data feed, on another.
 * It also syncs the contracts the engine trades: an underlying and its option chains, from IB.
 */

#include "../../utilities/log_level.hpp"
#include "../../model/object.hpp"
#include "ib_settings.hpp"
#include "../broker_account.hpp"
#include "../engine_gateway.hpp"
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adapters {

struct IbListing;
class IbTws;

class IbGateway : public Gateway {
  public:
    /** Through the TWS API. The account, the portfolios and the connection are checked here
     * before IB is asked. */
    explicit IbGateway(utilities::Logger* logger);
    ~IbGateway() override;

    IbGateway(const IbGateway&) = delete;
    IbGateway& operator=(const IbGateway&) = delete;

    /** Where connect() reaches IB: IbSettings::from_env() at construction; change it before
     * connecting. */
    IbSettings& default_setting() { return setting_; }
    const IbSettings& default_setting() const { return setting_; }

    /** Connect to default_setting(); throws when IB cannot be reached. */
    void connect() override;
    void disconnect() override;
    std::string send_order(const model::OrderRequest& req) override;
    void cancel_order(const model::CancelRequest& req) override;
    /** The account's values, and its positions with what each is worth, once connected; the
     * future fails if IB does not answer within a minute. */
    std::future<BrokerAccount> query_account() override;

    /** The portfolio's underlying, its hedge instrument, and its options expiring from today to
     * max_dte days on (New York dates), as IB lists them. Throws for a portfolio not in
     * core/portfolios.hpp, one whose underlying or hedge has no IB listing, or while not
     * connected; the future fails if IB does, or goes five minutes without answering (its first
     * query after a login can be that slow). Logs where it stands as it goes. One sync at a
     * time. */
    std::future<std::vector<model::ContractData>> sync_contracts(std::string_view portfolio,
                                                                     int max_dte) override;

    /** Orders from before a restart: the working ones get their updates and fills as before, and
     * no order id is used again. Before connecting. */
    void restore_orders(std::span<const model::OrderData> orders) override;
    /** After connect(): today's fills (the engine drops the ones it has), then the end of
     * restored orders that stopped working while the engine was down. The future fails if IB
     * does not answer within a minute. */
    std::future<void> catch_up() override;

    bool has_broker() const override { return true; }
    bool is_connected() const override;

  private:
    IbSettings setting_;
    std::unique_ptr<IbTws> tws_; // the connection (engine_gateway_ib.cpp)
};

} // namespace adapters
