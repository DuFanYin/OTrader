#pragma once

/** What the live gateways report of the account beyond orders and fills: a snapshot of its
 * values and its positions, with what each is worth. */

#include <string>
#include <vector>

namespace adapters {

/** One of the account's values as the broker states it (NetLiquidation, AvailableFunds, ...): a
 * number in a currency, or text (AccountType). */
struct AccountValue {
    std::string key;
    std::string value;
    std::string currency; // empty for text; BASE is the account's base currency
};

/** A position as the broker values it. Prices are per share or per option (not per contract). */
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

} // namespace adapters
