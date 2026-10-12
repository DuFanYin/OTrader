#pragma once

/**
 * Gateway: the engine's execution port, where its orders go and their updates and fills come
 * from (through the callbacks, into the engine's events). This base is the venue that is not
 * there: it connects to nothing, rejects every order (logged) and has no account to ask; a build
 * without IB runs live on it. IbGateway (Interactive Brokers) and BacktestGateway (fills at the
 * backtest's quotes) are the venues.
 *
 * What every venue reports (broker-specific handling stays inside the venue's gateway):
 * - a fill is one contract's: symbol, side, price and size of that contract. A combo order fills
 *   as its legs, one fill per leg under the combo's orderid, and has no fill of its own (IB's BAG
 *   execution, at the net price, is not reported as a fill);
 * - an order update carries the status and the traded quantity (for a combo, in combo units);
 * - every fill has a tradeid unique for the venue, the same when it is reported again.
 * The engine does not depend on arrival order: updates may repeat, arrive late or before their
 * fills, and fills may arrive in any order (Engine::book_order,
 * ExecutionEngine::process_order_event, PositionEngine::process_trade_event).
 */

#include "../utilities/base_engine.hpp"
#include "../utilities/log_level.hpp"
#include "../model/object.hpp"
#include "broker_account.hpp"
#include <functional>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adapters {

class Gateway : public utilities::BaseEngine {
  public:
    using OrderCallback = std::function<void(const model::OrderData&)>;
    using TradeCallback = std::function<void(const model::TradeData&)>;

    explicit Gateway(utilities::Logger* logger, const std::string& name = "Gateway");
    virtual ~Gateway() = default;

    /** Where order updates and fills go (the engine's events); set before connect(). */
    void set_callbacks(OrderCallback on_order, TradeCallback on_trade);
    /** An order's update, a fill: to where set_callbacks said (any thread the venue answers on). */
    void put_order(const model::OrderData& order) const;
    void put_trade(const model::TradeData& trade) const;
    /** Throws when the venue cannot be reached. */
    virtual void connect();
    virtual void disconnect() {}
    virtual bool is_connected() const { return false; }
    /** A broker is behind it: its account's positions are checked against the holdings. */
    virtual bool has_broker() const { return false; }

    /** The order's id; empty when it is rejected. */
    virtual std::string send_order(const model::OrderRequest& req);
    virtual void cancel_order(const model::CancelRequest& req);

    /** The account at the broker. Throws: there is none to ask. */
    virtual std::future<BrokerAccount> query_account();
    /** A portfolio's contracts from the broker. Throws: there is none to ask. */
    virtual std::future<std::vector<model::ContractData>>
    sync_contracts(std::string_view portfolio, int max_dte);
    /** Orders from before a restart, still working at the venue. */
    virtual void restore_orders(std::span<const model::OrderData> /*orders*/) {}
    /** After connect(): what happened while the engine was down. Throws: there is none to ask. */
    virtual std::future<void> catch_up();

    virtual void close() { disconnect(); }

  protected:
    OrderCallback on_order_;
    TradeCallback on_trade_;
};

} // namespace adapters
