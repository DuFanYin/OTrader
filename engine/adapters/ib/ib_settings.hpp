#pragma once

/** How the engine reaches IB Gateway / TWS, from the environment (entry_live --help lists it). */

#include <string>

namespace utilities {
struct BaseEngine;
} // namespace utilities

namespace adapters {

struct IbSettings {
    std::string host = "127.0.0.1";
    int port = 7497;
    int client_id = 1;           // orders; market data connects as client_id + 1
    std::string account;         // the one account to trade (required)
    /** Where a live account (not D...) must be named before the engine trades it: a file of
     * root's that only root may write, so only sudo makes it and no program run as a user (an
     * agent, say) can allow a live account. Not from the environment, for the same reason. */
    std::string live_account_file = "/etc/optra/live-account";
    int market_data_lines = 100; // quotes the account may stream at once
    int market_data_type = 1;    // 1 live; 3 delayed, for an account without a live subscription

    /** IB_HOST, IB_PORT, IB_CLIENT_ID, IB_ACCOUNT, IB_MARKET_DATA_LINES and IB_MARKET_DATA_TYPE
     * over the defaults; a value that is not a number is logged through log and ignored. */
    static IbSettings from_env(const utilities::BaseEngine& log);

    /** account is a paper account (D...), or the live one live_account_file names, that file
     * being root's and writable by root alone. */
    [[nodiscard]] bool is_account_allowed() const;
};

} // namespace adapters
