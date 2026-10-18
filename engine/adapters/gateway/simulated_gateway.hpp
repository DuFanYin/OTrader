#pragma once

/** The simulated market's venue (entry_simulate): backtest fills on replayed quotes, no broker. */

#include "adapters/gateway/backtest_gateway.hpp"
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adapters {

/** Fills at once when quotes allow, else on later minutes. The account is persisted in
 * account_file so a restart resumes it. */
class SimulatedGateway final : public BacktestGateway {
  public:
    using Clock = std::function<model::DateTime()>;

    static constexpr double kStartingCash = 100'000.0;

    SimulatedGateway(const model::Portfolios& portfolios, std::vector<std::string> files,
                     std::string account_file, Clock clock, double fee_rate);

    void connect() override;
    void disconnect() override { connected_ = false; }
    bool is_connected() const override { return connected_; }
    std::string send_order(const model::OrderRequest& req) override;
    /** The simulated venue keeps no orders across restarts, so these are cancelled. */
    void restore_orders(std::span<const model::Order> orders) override;
    std::future<core::BrokerAccount> query_account() override;
    std::future<std::vector<model::Contract>> sync_contracts(std::string_view portfolio,
                                                             int max_dte) override;

    /** Each minute (event thread): the waiting orders the new quotes allow, the account's prices,
     * and its options past their close settled at the underlying's price. */
    void on_minute();

  private:
    struct Position {
        double quantity = 0.0; // signed: shares, and options in contracts
        double cost = 0.0;     // what the open quantity cost, in USD
        double realized = 0.0;
    };

    /** A fill into the account (the fill callback's, ahead of the engine's). */
    void book(const std::string& symbol, double quantity, double price);
    void settle_expired();
    double mid(const std::string& symbol) const; // event thread: the portfolios' quotes
    void load_account();
    void save_account() const;

    std::vector<std::string> files_;
    std::string account_file_;
    Clock clock_;
    bool connected_ = false;
    bool booking_ = false; // the fill callback wraps the engine's once

    mutable std::mutex mutex_; // the account: written on the event thread, read by query_account
    double cash_ = kStartingCash;
    double fees_booked_ = 0.0;
    std::map<std::string, Position> positions_;
    std::map<std::string, double> marks_; // the latest mid of each contract held
};

} // namespace adapters
