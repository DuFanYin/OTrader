#pragma once

/**
 * Gateway: the broker port. This base is "no broker": it rejects every order; a build without IB
 * runs live on it. Venues are in adapters/.
 *
 * What every venue reports (broker-specific handling stays inside the venue):
 * - a fill is one contract's: symbol, side, price and size of that contract. A combo order fills
 *   as its legs, one fill per leg under the combo's order_id, and has no fill of its own (IB's BAG
 *   execution, at the net price, is not reported as a fill);
 * - an order update carries the status and the filled quantity (for a combo, in combo units);
 * - every fill has a fill_id unique for the venue, the same when it is reported again.
 * The engine does not depend on arrival order: updates may repeat, arrive late or before their
 * fills, and fills may arrive in any order (Main::book_order, BookkeepingEngine::update_order,
 * BookkeepingEngine::book_fill).
 */

#include "utilities/log_source.hpp"
#include "utilities/log_level.hpp"
#include "model/object.hpp"
#include <functional>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core {

/** NetLiquidation, AvailableFunds, ...; or text such as AccountType. */
struct AccountValue {
    std::string key;
    std::string value;
    std::string currency; // empty for text; BASE is the account's base currency
};

/** Prices are per share or per option, not per contract. */
struct AccountPosition {
    std::string symbol;    // the platform's
    double quantity = 0.0; // signed: shares, and options in contracts
    double market_price = 0.0;
    double market_value = 0.0;
    double average_cost = 0.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;
};

struct BrokerAccount {
    std::string account;
    std::string updated; // the broker's time of these values (HH:MM)
    std::vector<AccountValue> values;
    std::vector<AccountPosition> positions;
};

class Gateway {
  public:
    using OrderCallback = std::function<void(const model::Order&)>;
    using TradeCallback = std::function<void(const model::Fill&)>;

    explicit Gateway(utilities::Logger* logger, const std::string& name = "Gateway");
    virtual ~Gateway() = default;

    /** Set before connect(). */
    void set_callbacks(OrderCallback on_order, TradeCallback on_fill);
    /** Called on whatever thread the venue answers on. */
    void put_order(const model::Order& order) const;
    void put_fill(const model::Fill& fill) const;
    /** Throws when the venue cannot be reached. */
    virtual void connect();
    virtual void disconnect() {}
    virtual bool is_connected() const { return false; }
    /** If true, broker positions are checked against the holdings. */
    virtual bool has_broker() const { return false; }

    /** Empty when rejected. */
    virtual std::string send_order(const model::OrderRequest& req);
    virtual void cancel_order(const model::CancelRequest& req);

    virtual std::future<BrokerAccount> query_account();
    virtual std::future<std::vector<model::Contract>>
    sync_contracts(std::string_view portfolio, int max_dte);
    /** Orders from before a restart, still working at the venue. */
    virtual void restore_orders(std::span<const model::Order> /*orders*/) {}
    /** After connect(): replay what happened while the engine was down. */
    virtual std::future<void> catch_up();

    virtual void close() { disconnect(); }

    /** What a venue's connection logs through. */
    [[nodiscard]] auto log_source() const -> const utilities::LogSource& { return log_; }

  protected:
    utilities::LogSource log_;
    OrderCallback on_order_;
    TradeCallback on_trade_;
};

} // namespace core
