#pragma once

/** Shared by IbGateway and IbMarketData: IB settings, contract translation, the connection. */

#include "model/object.hpp"
#include "model/portfolio_spec.hpp"
#include "utilities/log_level.hpp"
#include "utilities/log_source.hpp"
#include "utilities/mpsc_queue.hpp"

#include "Contract.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace adapters {

// ---- Settings
/** How the engine reaches IB Gateway / TWS, from the environment (entry_live --help lists it). */
struct IbSettings {
    std::string host = "127.0.0.1";
    int port = 7497;
    int client_id = 1;           // orders; market data connects as client_id + 1
    std::string account;         // the one account to trade (required)
    /** Root-only file that must name a live (non-D) account before it is traded, so no user-run
     * program can enable live trading; deliberately not from the environment. */
    std::string live_account_file = "/etc/optra/live-account";
    /** Root-only, like live_account_file: the strategies a live account's engine may run, a line
     * each, "<class> <strategy code> <name>=<value> ..." (the code: its library's .code
     * "strategy"; the setting's other values its defaults). */
    std::string live_strategies_file = "/etc/optra/live-strategies";
    int market_data_lines = 100; // quotes the account may stream at once
    int market_data_type = 1;    // 1 live; 3 delayed, for an account without a live subscription

    /** IB_* variables over the defaults; a non-numeric value is logged and ignored. */
    static IbSettings from_env(const utilities::LogSource& log);

    /** Paper (D...), or the live account named in a root-owned, root-only live_account_file. */
    [[nodiscard]] bool is_account_allowed() const;
    /** A live account (not D...): its engine runs only the strategies of live_strategies(). */
    [[nodiscard]] bool is_live_account() const { return !account.empty() && !account.starts_with('D'); }
    /** live_strategies_file's lines; none when it is missing or not root's alone. */
    [[nodiscard]] std::vector<std::string> live_strategies() const;
};

// ---- Contracts
/**
 * The only place IB identifiers are translated to and from platform symbols:
 *   an underlying  SPY STK, SPX IND              <->  SPY, SPX (its ticker)
 *   an option      SPX OPT SPXW 20261016 C 5800  <->  SPXW-20261016-C-5800 (root: trading class)
 */

/** How IB lists an underlying the engine trades. */
struct IbListing {
    std::string_view symbol;
    std::string_view sec_type; // STK or IND
    std::string_view exchange; // SMART, or the exchange an index is listed on
};

inline constexpr std::array kIbListings{
    IbListing{"SPY", "STK", "SMART"},
    IbListing{"SPX", "IND", "CBOE"},
    IbListing{"QQQ", "STK", "SMART"},
    IbListing{"IWM", "STK", "SMART"},
    IbListing{"XSP", "IND", "CBOE"},
};

/** How IB lists `symbol`, or nullptr. */
constexpr auto find_listing(std::string_view symbol) -> const IbListing* {
    const auto it = std::ranges::find(kIbListings, symbol, &IbListing::symbol);
    return it != kIbListings.end() ? &*it : nullptr;
}

// Every portfolio's underlying (an index as IND, a stock as STK) and hedge is listed here.
static_assert(std::ranges::all_of(model::kPortfolios, [](const model::PortfolioSpec& p) {
    const IbListing* underlying = find_listing(p.underlying);
    return underlying != nullptr && find_listing(p.hedge) != nullptr &&
           (underlying->sec_type == "IND") == (p.underlying_product == model::Product::INDEX);
}));

/** The platform's symbol for an IB contract. */
std::string platform_symbol(const Contract& contract);

/** An option, or an underlying per kIbListings (else a stock on SMART). */
Contract ib_contract(const std::string& symbol);

/** The IB contract for a combo: a BAG of its legs, by contract id. */
Contract ib_combo_contract(std::span<const model::Leg> legs, const std::string& trading_class);

/** The platform's contract for the underlying IB describes. */
model::Contract underlying_contract(const ContractDetails& details);

/** Portfolio from the trading class; name is the OCC code (IB's local symbol). */
model::Contract option_contract(const ContractDetails& details,
                                const std::string& underlying_symbol);

/** The expirations (IB's YYYYMMDD) from `from` to max_dte days later, in order. */
std::vector<std::string> expiries_within(const std::set<std::string>& expirations,
                                         std::chrono::year_month_day from, int max_dte);

// ---- Session
/**
 * One TWS API connection with a message thread and auto-reconnect. send() serializes requests
 * (the TWS socket is not thread-safe); latency-sensitive requests go through a lock-free queue to
 * the message thread. Derived classes must call stop() in their destructor: the thread calls
 * their hooks.
 */

class IbSession : public DefaultEWrapper {
  public:
    /** owner: log lines go under its name. */
    explicit IbSession(const utilities::LogSource& owner);
    ~IbSession() override;

    IbSession(const IbSession&) = delete;
    IbSession& operator=(const IbSession&) = delete;

    /** False when IB cannot be reached; afterwards drops are retried every few seconds. */
    bool start(const std::string& host, int port, int client_id);
    void stop();
    [[nodiscard]] bool connected() const { return connected_; }

  protected:
    /** Run request(EClientSocket&) while connected; false if not. Any thread. */
    template <typename Request> bool send(Request&& request) {
        std::scoped_lock lock(send_mutex_);
        if (!connected_) {
            return false;
        }
        std::forward<Request>(request)(socket_);
        return true;
    }

    /** FIFO on the message thread, so the caller never waits on the socket; jobs left at stop()
     * still run (their send() fails). Any thread. */
    void queue_on_message_thread(std::function<void()> job);

    /** After each connect, before its first message: on start()'s caller the first time, then on
     * the message thread. */
    virtual void on_connected() {}
    /** The connection dropped (message thread); it is being retried. */
    virtual void on_disconnected() {}
    /** After a reconnect, once on_connected has run (message thread). */
    virtual void on_reconnected() {}
    /** After each message batch, and at least every 100 ms (message thread). */
    virtual void on_wake() {}
    /** An error about one request (id: its order or request id); the default logs it. */
    virtual void on_error(int id, int code, const std::string& message);

    [[nodiscard]] const utilities::LogSource& owner() const { return owner_; }
    void log(const std::string& message, utilities::LogLevel level) const { owner_.write_log(message, level); }

  private:
    void error(int id, long long errorTimeMs, int errorCode, const std::string& errorString,
               const std::string& advancedOrderRejectJson) override;
    void connectionClosed() override;

    bool open();
    void close();
    void run(const std::stop_token& st);
    /** The jobs queued so far, in order (message thread, or stop()). */
    void run_queued_jobs();

    struct Job {
        std::function<void()> run;
        std::atomic<Job*> next{nullptr};
    };

    const utilities::LogSource& owner_;
    EReaderOSSignal signal_;
    EClientSocket socket_;
    std::unique_ptr<EReader> reader_;
    std::mutex send_mutex_;
    utilities::MpscQueue<Job> jobs_; // queue_on_message_thread's
    std::atomic<bool> connected_{false};
    std::atomic<bool> reconnecting_{false};
    std::string host_;
    int port_ = 0;
    int client_id_ = 0;
    std::jthread thread_;
};

} // namespace adapters
