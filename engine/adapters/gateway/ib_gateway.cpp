/** IbGateway over the TWS C++ API; callbacks run on the session's message thread. */

#include "adapters/gateway/ib_gateway.hpp"
#include "model/portfolio_spec.hpp"
#include "model/constant.hpp"
#include "model/utility.hpp"
#include "adapters/ib_api/ib_api.hpp"

#include "CommonDefs.h"
#include "Contract.h"
#include "Decimal.h"
#include "Execution.h"
#include "Order.h"
#include "OrderCancel.h"
#include "OrderState.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <format>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace adapters {

namespace {

// IB's order status. SUBMITTING is an order IB has not taken yet; PreSubmitted (held: a simulated
// order, or until the open) and PendingCancel are taken, working.
model::Status status_from_ib(const std::string& status) {
    static const std::unordered_map<std::string, model::Status> m = {
        {"ApiPending", model::Status::SUBMITTING},
        {"PendingSubmit", model::Status::SUBMITTING},
        {"PreSubmitted", model::Status::WORKING},
        {"Submitted", model::Status::WORKING},
        {"ApiCancelled", model::Status::CANCELLED},
        {"PendingCancel", model::Status::WORKING},
        {"Cancelled", model::Status::CANCELLED},
        {"Filled", model::Status::FILLED},
        {"Inactive", model::Status::REJECTED},
        {"PartiallyFilled", model::Status::PARTLY_FILLED},
    };
    auto it = m.find(status);
    return it != m.end() ? it->second : model::Status::SUBMITTING;
}

std::string direction_to_ib(model::Side d) {
    return d == model::Side::SELL ? "SELL" : "BUY";
}

// IB's order action, or an execution's side (BOT, SLD).
model::Side direction_from_ib(const std::string& action) {
    return action == "SELL" || action == "SLD" ? model::Side::SELL : model::Side::BUY;
}

// IB's order type.
std::string ordertype_to_ib(model::OrderType t) {
    return t == model::OrderType::MARKET ? "MKT" : "LMT";
}

model::OrderType ordertype_from_ib(const std::string& t) {
    return t == "MKT" ? model::OrderType::MARKET : model::OrderType::LIMIT;
}

/** How long IB may take to answer a catch-up or an account snapshot. */
constexpr auto kRequestTimeout = std::chrono::seconds(60);
/** The exception a request fails with: a timeout or a dropped connection says which (gRPC tells
 * them apart), anything else is a plain error. */
auto request_failure(const std::string& what, std::optional<std::errc> code) -> std::exception_ptr {
    return code ? std::make_exception_ptr(std::system_error(std::make_error_code(*code), what))
                : std::make_exception_ptr(std::runtime_error(what));
}

/** How long a contract sync may go without an answer: IB's first contract query after a login
 * can be silent for minutes; later ones answer at once. */
constexpr auto kContractSyncTimeout = std::chrono::minutes(5);
/** How often a contract sync logs where it stands. */
constexpr auto kContractSyncLogInterval = std::chrono::seconds(10);

/** The IB order id an engine order id is, or none when it is not one. */
auto ib_order_id(const std::string& order_id) -> std::optional<int> {
    int id = 0;
    if (std::from_chars(order_id.data(), order_id.data() + order_id.size(), id).ec != std::errc{}) {
        return std::nullopt;
    }
    return id;
}

// Request ids start above any order id, so an error's id tells which of the two it is about.
constexpr int kFirstRequestId = 1'000'000;
/** The execution counts toward the order's filled quantity (used by the catch-up): a combo's BAG
 * execution (not its legs'), or a contract's own order's execution. */
auto fills_order_quantity(const model::Order& order, const Contract& contract) -> bool {
    return !order.is_combo() || contract.secType == "BAG";
}

} // namespace

// IbTws: the gateway's TWS connection

class IbTws final : public IbSession {
  public:
    explicit IbTws(const core::Gateway& gateway)
        : IbSession(gateway.log_source()), gateway_(gateway) {}
    ~IbTws() override { stop(); } // a sync still running fails with a broken promise

    IbTws(const IbTws&) = delete;
    IbTws& operator=(const IbTws&) = delete;

    auto connect(const IbSettings& settings) -> bool {
        {
            std::scoped_lock lock(mutex_);
            account_ = settings.account;
            client_id_ = settings.client_id;
            managed_accounts_.reset();
        }
        account_managed_.store(false, std::memory_order_release);
        if (!start(settings.host, settings.port, settings.client_id)) {
            return false;
        }
        // IB names the accounts its login manages at once: IB_ACCOUNT must be one of them (a
        // live gateway's login does not manage the paper account).
        std::unique_lock lock(mutex_);
        const bool named = accounts_named_.wait_for(lock, kRequestTimeout,
                                                    [this] { return managed_accounts_.has_value(); });
        const bool managed = named && std::ranges::contains(*managed_accounts_, account_);
        lock.unlock();
        if (!managed) {
            stop();
            throw std::runtime_error(
                named ? "IB's login does not manage IB_ACCOUNT: not connected (is this the paper"
                        " gateway?)"
                      : "IB did not name the accounts its login manages: not connected");
        }
        return true;
    }

    void disconnect() {
        stop();
        fail_requests("disconnected");
    }

    [[nodiscard]] auto is_connected() const -> bool { return connected(); }

    auto send_order(const model::OrderRequest& req) -> std::string {
        if (!connected()) {
            log("IB send_order failed: not connected", utilities::ERROR);
            return "";
        }
        if (!account_managed_.load(std::memory_order_acquire)) {
            log("IB send_order refused: IB's login does not manage IB_ACCOUNT", utilities::ERROR);
            return "";
        }
        if (req.type != model::OrderType::LIMIT && req.type != model::OrderType::MARKET) {
            log("IB send_order failed: unsupported order type", utilities::ERROR);
            return "";
        }
        if (req.legs.empty()) {
            log("IB send_order failed: an order needs a leg", utilities::ERROR);
            return "";
        }
        const int ib_id = next_order_id_.fetch_add(1, std::memory_order_relaxed);
        const std::string order_id = std::to_string(ib_id);
        model::Order od = req.create_order(order_id);
        emit_order(od);
        // Built and sent on the message thread, which owns the order book, so the engine only
        // hands the request over. Booked before sending so IB's answers find it.
        queue_on_message_thread(
            [this, req, od = std::move(od), ib_id]() mutable { place(req, std::move(od), ib_id); });
        return order_id;
    }

    void cancel_order(const model::CancelRequest& req) {
        const std::optional<int> ib_id = ib_order_id(req.order_id);
        if (!ib_id) {
            log("IB cancel_order: not an IB order id: " + req.order_id, utilities::ERROR);
            return;
        }
        const int id = *ib_id;
        OrderCancel oc;
        oc.manualOrderCancelTime = std::format( // UTC
            "{:%Y%m%d-%H:%M:%S}",
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        queue_on_message_thread([this, id, oc] {
            send([&](EClientSocket& s) { s.cancelOrder(id, oc); });
        });
    }

    // ---- The account: one snapshot of IB's account window (its values, and what each position is
    // worth), subscribed to until it has arrived whole.

    auto query_account() -> std::future<core::BrokerAccount> {
        if (!connected()) {
            throw std::runtime_error("Connect the IB gateway first");
        }
        std::string code;
        {
            std::scoped_lock lock(mutex_);
            code = account_;
        }
        if (code.empty()) {
            throw std::runtime_error("IB has not named the account yet (IB_ACCOUNT)");
        }
        std::scoped_lock lock(account_mutex_);
        if (account_query_) {
            throw std::runtime_error("Already asking IB for the account");
        }
        account_query_.emplace();
        account_query_->deadline = std::chrono::steady_clock::now() + kRequestTimeout;
        account_query_->snapshot.account = code;
        auto future = account_query_->done.get_future();
        send([&](EClientSocket& s) { s.reqAccountUpdates(true, code); });
        return future;
    }

    void updateAccountValue(const std::string& key, const std::string& val,
                            const std::string& currency, const std::string& accountName) override {
        std::scoped_lock lock(account_mutex_);
        if (account_query_ && accountName == account_query_->snapshot.account) {
            account_query_->snapshot.values.push_back(
                {.key = key, .value = val, .currency = currency});
        }
    }

    void updatePortfolio(const Contract& contract, Decimal position, double marketPrice,
                         double marketValue, double averageCost, double unrealizedPNL,
                         double realizedPNL, const std::string& accountName) override {
        std::scoped_lock lock(account_mutex_);
        if (!account_query_ || accountName != account_query_->snapshot.account) {
            return;
        }
        // IB's average cost is per contract: an option's is its price times the multiplier.
        double multiplier = 1.0;
        const std::string& m = contract.multiplier;
        if (std::from_chars(m.data(), m.data() + m.size(), multiplier).ec != std::errc{} ||
            multiplier <= 0.0) {
            multiplier = 1.0;
        }
        account_query_->snapshot.positions.push_back(
            {.symbol = platform_symbol(contract),
             .quantity = DecimalFunctions::decimalToDouble(position),
             .market_price = marketPrice,
             .market_value = marketValue,
             .average_cost = averageCost / multiplier,
             .unrealized_pnl = unrealizedPNL,
             .realized_pnl = realizedPNL});
    }

    void updateAccountTime(const std::string& timeStamp) override {
        std::scoped_lock lock(account_mutex_);
        if (account_query_) {
            account_query_->snapshot.updated = timeStamp;
        }
    }

    void accountDownloadEnd(const std::string& accountName) override {
        std::scoped_lock lock(account_mutex_);
        if (!account_query_ || accountName != account_query_->snapshot.account) {
            return;
        }
        send([&](EClientSocket& s) { s.reqAccountUpdates(false, accountName); });
        account_query_->done.set_value(std::move(account_query_->snapshot));
        account_query_.reset();
    }

    // ---- Contract sync: the underlying, then its option parameters (the expirations), then the
    // contracts of each expiry in the window; the hedge instrument alongside. Callbacks on the
    // message thread.

    auto sync_contracts(std::string_view portfolio, const IbListing& listing,
                        const IbListing* hedge, int max_dte)
        -> std::future<std::vector<model::Contract>> {
        std::scoped_lock lock(sync_mutex_);
        if (sync_) {
            throw std::runtime_error("A contract sync is already running");
        }
        Sync& s = sync_.emplace();
        s.portfolio = portfolio;
        s.listing = listing;
        s.from = model::calendar_of(portfolio).date_of(std::chrono::system_clock::now());
        s.max_dte = max_dte;
        s.deadline = std::chrono::steady_clock::now() + kContractSyncTimeout;
        s.next_log = std::chrono::steady_clock::now() + kContractSyncLogInterval;
        s.underlying_req = next_request_id();
        auto done = s.done.get_future();
        if (!send([&](EClientSocket& socket) {
                socket.reqContractDetails(s.underlying_req,
                                          ib_contract(std::string(listing.symbol)));
            })) {
            sync_.reset();
            throw std::runtime_error("Not connected to IB");
        }
        if (hedge != nullptr) {
            s.hedge = *hedge;
            s.hedge_req = next_request_id();
            send([&](EClientSocket& socket) {
                socket.reqContractDetails(s.hedge_req, ib_contract(std::string(hedge->symbol)));
            });
        }
        log(std::format("Syncing {} contracts from IB (expiries up to {} days on)", portfolio,
                        max_dte),
            utilities::INFO);
        return done;
    }

    void contractDetails(int reqId, const ContractDetails& details) override {
        std::scoped_lock lock(sync_mutex_);
        if (!sync_) {
            return;
        }
        sync_->deadline = std::chrono::steady_clock::now() + kContractSyncTimeout; // IB is answering
        if (reqId == sync_->underlying_req && sync_->underlying_con_id == 0) {
            sync_->underlying_con_id = details.contract.conId;
            sync_->contracts.push_back(underlying_contract(details));
            sync_->underlying_symbol = sync_->contracts.back().symbol;
        } else if (reqId == sync_->hedge_req && !sync_->hedge_found) {
            sync_->contracts.push_back(underlying_contract(details));
            sync_->hedge_found = true;
        } else if (sync_->pending.contains(reqId)) {
            sync_->contracts.push_back(option_contract(details, sync_->underlying_symbol));
        }
    }

    void contractDetailsEnd(int reqId) override {
        std::scoped_lock lock(sync_mutex_);
        if (!sync_) {
            return;
        }
        sync_->deadline = std::chrono::steady_clock::now() + kContractSyncTimeout;
        const auto not_listed = [](const IbListing& l) {
            return std::format("IB lists no {} {} on {}", l.symbol, l.sec_type, l.exchange);
        };
        const IbListing& u = sync_->listing;
        if (reqId == sync_->underlying_req) {
            if (sync_->underlying_con_id == 0) {
                fail_sync(not_listed(u));
                return;
            }
            sync_->params_req = next_request_id();
            send([&](EClientSocket& s) {
                s.reqSecDefOptParams(sync_->params_req, std::string(u.symbol), "",
                                     std::string(u.sec_type), sync_->underlying_con_id);
            });
        } else if (reqId == sync_->hedge_req) {
            if (!sync_->hedge_found) {
                fail_sync(not_listed(*sync_->hedge));
                return;
            }
            sync_->hedge_req = 0; // answered
            finish_sync();
        } else if (const auto it = sync_->pending.find(reqId); it != sync_->pending.end()) {
            const std::string expiry = it->second;
            sync_->pending.erase(it);
            log(std::format("{} options expiring {}: in ({} of {} expiries, {} contracts so far)",
                            sync_->portfolio, expiry,
                            sync_->expiries.size() - sync_->pending.size(),
                            sync_->expiries.size(), sync_->contracts.size()),
                utilities::INFO);
            finish_sync();
        }
    }

    void securityDefinitionOptionalParameter(int reqId, const std::string& exchange,
                                             int /*underlyingConId*/,
                                             const std::string& tradingClass,
                                             const std::string& /*multiplier*/,
                                             const std::set<std::string>& expirations,
                                             const std::set<double>& /*strikes*/) override {
        std::scoped_lock lock(sync_mutex_);
        // One line per exchange: SMART's, else the first listing the portfolio's class.
        if (!sync_ || reqId != sync_->params_req || tradingClass != sync_->portfolio ||
            (exchange != "SMART" && !sync_->exchange.empty())) {
            return;
        }
        sync_->exchange = exchange;
        sync_->expiries = expiries_within(expirations, sync_->from, sync_->max_dte);
    }

    void securityDefinitionOptionalParameterEnd(int reqId) override {
        std::scoped_lock lock(sync_mutex_);
        if (!sync_ || reqId != sync_->params_req) {
            return;
        }
        sync_->deadline = std::chrono::steady_clock::now() + kContractSyncTimeout;
        const IbListing& u = sync_->listing;
        if (sync_->expiries.empty()) {
            fail_sync(std::format("IB lists no {} options expiring within {} days",
                                  sync_->portfolio, sync_->max_dte));
            return;
        }
        log(std::format("Asking IB for the {} options of {} expiries, {} to {}", sync_->portfolio,
                        sync_->expiries.size(), sync_->expiries.front(), sync_->expiries.back()),
            utilities::INFO);
        for (const std::string& expiry : sync_->expiries) {
            const int id = next_request_id();
            sync_->pending.emplace(id, expiry);
            Contract c;
            c.symbol = u.symbol;
            c.secType = "OPT";
            c.exchange = sync_->exchange;
            c.currency = "USD";
            c.tradingClass = sync_->portfolio;
            c.lastTradeDateOrContractMonth = expiry;
            send([&](EClientSocket& s) { s.reqContractDetails(id, c); });
        }
        sync_->options_requested = true;
    }

    // ---- Orders and fills
    void nextValidId(int orderId) override { raise_next_order_id(orderId); }

    void managedAccounts(const std::string& accountsList) override {
        std::vector<std::string> accounts;
        for (const auto account : accountsList | std::views::split(',')) {
            if (!account.empty()) {
                accounts.emplace_back(account.begin(), account.end());
            }
        }
        {
            std::scoped_lock lock(mutex_);
            const bool managed = std::ranges::contains(accounts, account_);
            if (managed) {
                log("Trading account " + account_ + " (IB_ACCOUNT)", utilities::INFO);
            } else {
                // After a reconnect too: from now on every order is refused.
                log("IB's login does not manage IB_ACCOUNT: orders are refused", utilities::ERROR);
            }
            managed_accounts_ = std::move(accounts);
            account_managed_.store(managed, std::memory_order_release);
        }
        accounts_named_.notify_all();
    }

    void orderStatus(int orderId, const std::string& status, Decimal filled, Decimal remaining,
                     double avgFillPrice, long long permId, int /*parentId*/, double lastFillPrice,
                     int clientId, const std::string& whyHeld, double /*mktCapPrice*/) override {
        if (owner().enabled(utilities::DEBUG)) {
            log(std::format("IB orderStatus id={} status={} filled={} remaining={} "
                            "avgFillPrice={} lastFillPrice={} permId={} clientId={} whyHeld={}",
                            orderId, status, DecimalFunctions::decimalToDouble(filled),
                            DecimalFunctions::decimalToDouble(remaining), avgFillPrice,
                            lastFillPrice, permId, clientId, whyHeld),
                utilities::DEBUG);
        }
        const std::string order_id = std::to_string(orderId);
        const model::Status st = status_from_ib(status);
        const double fill = DecimalFunctions::decimalToDouble(filled);
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            return;
        }
        if (it->second.status == st && it->second.filled == fill) {
            return; // as last reported
        }
        it->second.filled = fill;
        it->second.status = st;
        const model::Order updated = it->second;
        if (!updated.is_active()) {
            forget(order_id);
        }
        emit_order(updated);
    }

    void openOrder(int orderId, const Contract& contract, const Order& order,
                   const OrderState& orderState) override {
        if (owner().enabled(utilities::DEBUG)) {
            std::string legs;
            if (contract.comboLegs) {
                for (const auto& leg : *contract.comboLegs) {
                    legs += std::format(" [conId={} action={} ratio={}]", leg->conId,
                                        leg->action, leg->ratio);
                }
            }
            log(std::format("IB openOrder id={} secType={} symbol={} localSymbol={} conId={} "
                            "action={} orderType={} totalQuantity={} lmtPrice={} state={}{}",
                            orderId, contract.secType, contract.symbol, contract.localSymbol,
                            contract.conId, order.action, order.orderType,
                            DecimalFunctions::decimalToDouble(order.totalQuantity),
                            order.lmtPrice, orderState.status, legs),
                utilities::DEBUG);
        }
        const std::string order_id = std::to_string(orderId);
        {
            std::scoped_lock lock(catch_up_mutex_);
            if (catch_up_ && catch_up_->step == Step::OpenOrders) {
                catch_up_->open.insert(order_id);
            }
        }
        if (completed_orders_.contains(order_id) || orders_.contains(order_id)) {
            return; // ours, already known
        }
        // Another client's, or from before this engine knew of it: shown and cancelled, as one leg.
        const model::Side side = direction_from_ib(order.action);
        const double limit = order.orderType == "LMT" ? order.lmtPrice : 0;
        model::Order od;
        od.symbol = platform_symbol(contract);
        od.order_id = order_id;
        od.type = ordertype_from_ib(order.orderType);
        od.quantity = DecimalFunctions::decimalToDouble(order.totalQuantity);
        od.price = side == model::Side::BUY ? limit : -limit;
        od.status = model::Status::SUBMITTING;
        od.legs = {{.ratio = 1, .side = side, .symbol = od.symbol}};
        orders_[order_id] = od;
        emit_order(od);
    }

    void execDetails(int reqId, const Contract& contract, const Execution& execution) override {
        if (owner().enabled(utilities::DEBUG)) {
            log(std::format("IB execDetails reqId={} orderId={} secType={} symbol={} "
                            "localSymbol={} conId={} side={} shares={} price={} cumQty={} "
                            "avgPrice={} execId={} permId={} time={}",
                            reqId, execution.orderId, contract.secType, contract.symbol,
                            contract.localSymbol, contract.conId, execution.side,
                            DecimalFunctions::decimalToDouble(execution.shares), execution.price,
                            DecimalFunctions::decimalToDouble(execution.cumQty),
                            execution.avgPrice, execution.execId, execution.permId,
                            execution.time),
                utilities::DEBUG);
        }
        const std::string order_id = std::to_string(execution.orderId);
        const double quantity = DecimalFunctions::decimalToDouble(execution.shares);
        if (const model::Order* order = known_order(order_id);
            order != nullptr && fills_order_quantity(*order, contract)) {
            std::scoped_lock lock(catch_up_mutex_);
            if (catch_up_) {
                catch_up_->filled[order_id] += quantity;
            }
        }
        // A fill is a contract's. A BAG's own execution (the combo at its net price, before its
        // legs') is IB's record of the combo, not a contract filled: its legs are.
        if (contract.secType == "BAG") {
            return;
        }
        model::Fill td;
        td.symbol = platform_symbol(contract);
        td.side = direction_from_ib(execution.side);
        td.order_id = order_id;
        td.fill_id = execution.execId;
        td.price = execution.price;
        td.quantity = quantity;
        emit_trade(td);
    }

    void execDetailsEnd(int reqId) override {
        std::scoped_lock lock(catch_up_mutex_);
        if (catch_up_ && catch_up_->step == Step::Executions &&
            reqId == catch_up_->executions_req) {
            enter(Step::OpenOrders);
        }
    }

    void openOrderEnd() override {
        std::scoped_lock lock(catch_up_mutex_);
        if (!catch_up_ || catch_up_->step != Step::OpenOrders) {
            return; // the list IB sends on its own at connect
        }
        // An unconfirmed order IB no longer works ended while we were away: filled as far as
        // today's fills say, cancelled (or expired) for the rest.
        std::vector<model::Order> ended;
        for (const std::string& order_id : unconfirmed_) {
            const auto it = orders_.find(order_id);
            if (catch_up_->open.contains(order_id) || it == orders_.end()) {
                continue;
            }
            model::Order& order = it->second;
            order.filled = std::max(order.filled, catch_up_->filled[order_id]);
            order.status = order.filled >= order.quantity ? model::Status::FILLED
                                                        : model::Status::CANCELLED;
            ended.push_back(order);
            forget(order_id);
        }
        unconfirmed_.clear();
        for (const model::Order& order : ended) {
            emit_order(order);
        }
        log(std::format("Caught up with IB: {} orders ended meanwhile", ended.size()), utilities::INFO);
        catch_up_->done.set_value();
        catch_up_.reset();
    }

    // ---- After a restart or a reconnect: the orders from before, then catching up once connected
    /** Any thread: the ids now; the orders onto the message thread's book (before the messages
     * after it, the catch-up's among them). */
    void restore_orders(std::span<const model::Order> orders) {
        for (const model::Order& order : orders) {
            if (const std::optional<int> id = ib_order_id(order.order_id)) {
                raise_next_order_id(*id + 1); // never an id used before
            }
        }
        queue_on_message_thread(
            [this, restored = std::vector<model::Order>(orders.begin(), orders.end())] {
                for (const model::Order& order : restored) {
                    if (!order.is_active()) {
                        completed_orders_[order.order_id] = order;
                        continue;
                    }
                    orders_[order.order_id] = order;
                    unconfirmed_.insert(order.order_id);
                }
            });
    }

    auto catch_up() -> std::future<void> {
        if (!connected()) {
            throw std::runtime_error("Connect the IB gateway first");
        }
        std::scoped_lock lock(catch_up_mutex_);
        if (catch_up_) {
            throw std::runtime_error("Already catching up with IB");
        }
        catch_up_.emplace();
        catch_up_->deadline = std::chrono::steady_clock::now() + kRequestTimeout;
        auto future = catch_up_->done.get_future();
        enter(Step::Executions);
        return future;
    }

  private:
    enum class Step : std::uint8_t { Executions, OpenOrders };

    /** A catch-up under way: each step asks IB, and its end marker moves on to the next. */
    struct CatchUp {
        Step step = Step::Executions;
        std::chrono::steady_clock::time_point deadline;
        std::promise<void> done;
        int executions_req = 0;
        std::unordered_set<std::string> open;           // the orders IB still works
        std::unordered_map<std::string, double> filled; // by today's fills, per order
    };

    /** Ask for a step's answers; catch_up_mutex_ held. */
    void enter(Step step) {
        catch_up_->step = step;
        switch (step) {
        case Step::Executions: {
            ExecutionFilter filter; // today's
            {
                std::scoped_lock lock(mutex_);
                filter.m_clientId = client_id_; // the orders this connection placed
            }
            const int id = next_request_id();
            catch_up_->executions_req = id;
            send([&](EClientSocket& s) { s.reqExecutions(id, filter); });
            break;
        }
        case Step::OpenOrders:
            send([](EClientSocket& s) { s.reqOpenOrders(); });
            break;
        }
    }

    /** An account snapshot under way (query_account). */
    struct AccountQuery {
        std::chrono::steady_clock::time_point deadline;
        std::promise<core::BrokerAccount> done;
        core::BrokerAccount snapshot;
    };

    /** Fail the account snapshot under way, if any; account_mutex_ held. */
    void fail_account(const std::string& reason, std::optional<std::errc> code = std::nullopt) {
        if (!account_query_) {
            return;
        }
        log("Asking IB for the account: " + reason, utilities::ERROR);
        account_query_->done.set_exception(
            request_failure("Asking IB for the account: " + reason, code));
        account_query_.reset();
    }

    /** Fail every request under way: no answer comes any more. */
    void fail_requests(const std::string& reason) {
        {
            std::scoped_lock lock(sync_mutex_);
            fail_sync(reason, std::errc::not_connected);
        }
        {
            std::scoped_lock lock(catch_up_mutex_);
            fail_catch_up(reason, std::errc::not_connected);
        }
        std::scoped_lock lock(account_mutex_);
        fail_account(reason, std::errc::not_connected);
    }

    /** Fail the catch-up under way, if any; catch_up_mutex_ held. */
    void fail_catch_up(const std::string& reason, std::optional<std::errc> code = std::nullopt) {
        if (!catch_up_) {
            return;
        }
        log("Catching up with IB: " + reason, utilities::ERROR);
        catch_up_->done.set_exception(request_failure("Catching up with IB: " + reason, code));
        catch_up_.reset();
    }

    /** An order of the engine's, working or done (message thread). */
    auto known_order(const std::string& order_id) const -> const model::Order* {
        if (const auto it = orders_.find(order_id); it != orders_.end()) {
            return &it->second;
        }
        const auto it = completed_orders_.find(order_id);
        return it != completed_orders_.end() ? &it->second : nullptr;
    }

    struct Sync {
        std::string_view portfolio; // the options' trading class
        IbListing listing;          // the underlying's
        std::chrono::year_month_day from{};
        int max_dte = 0;
        std::chrono::steady_clock::time_point deadline;
        std::promise<std::vector<model::Contract>> done;
        std::vector<model::Contract> contracts;
        std::string underlying_symbol;
        int underlying_con_id = 0;
        int underlying_req = 0;
        int params_req = 0;
        std::string exchange; // the options' exchange the expiries came from
        std::vector<std::string> expiries;
        std::unordered_map<int, std::string> pending; // expiries' requests still answering
        bool options_requested = false;
        std::chrono::steady_clock::time_point next_log; // when to log where it stands
        std::optional<IbListing> hedge; // the hedge instrument's, when not the underlying
        int hedge_req = 0;              // 0 once answered, or with no hedge instrument to ask for
        bool hedge_found = false;
    };

    void on_error(int id, int code, const std::string& message) override {
        {
            std::scoped_lock lock(sync_mutex_);
            if (sync_ && (id == sync_->underlying_req || id == sync_->params_req ||
                          (sync_->hedge_req != 0 && id == sync_->hedge_req) ||
                          sync_->pending.contains(id))) {
                fail_sync(std::format("IB {}: {}", code, message));
                return;
            }
        }
        if (code == 202) {
            return; // an order cancelled: orderStatus reports it
        }
        IbSession::on_error(id, code, message);
        reject_order(id, code, message);
    }

    /** IB's error about an order of ours: one it never took (still SUBMITTING), or one it
     * rejects outright (201), is REJECTED. Warnings (399, 21xx) leave it be. */
    void reject_order(int id, int code, const std::string& message) {
        if (code == 399 || (code >= 2100 && code < 2200)) {
            return;
        }
        const std::string order_id = std::to_string(id);
        const auto it = orders_.find(order_id);
        if (it == orders_.end() ||
            (it->second.status != model::Status::SUBMITTING && code != 201)) {
            return;
        }
        it->second.status = model::Status::REJECTED;
        const model::Order rejected = it->second;
        forget(order_id);
        log(std::format("Order {} rejected by IB ({}): {}", rejected.order_id, code, message),
            utilities::ERROR);
        emit_order(rejected);
    }

    void on_disconnected() override {
        fail_requests("the connection dropped");
        // The catch-up after reconnect settles working orders.
        for (const auto& [order_id, _] : orders_) {
            unconfirmed_.insert(order_id);
        }
    }

    // The fills and the order ends the drop hid: the same catch-up as after a restart.
    void on_reconnected() override {
        try {
            (void)catch_up();
        } catch (const std::exception& e) {
            log(std::format("Catching up with IB after the reconnect: {}", e.what()), utilities::ERROR);
        }
    }

    void on_wake() override {
        const auto now = std::chrono::steady_clock::now();
        const auto no_answer = [] {
            return std::format("IB did not answer within {} s", kRequestTimeout.count());
        };
        {
            std::scoped_lock lock(sync_mutex_);
            if (sync_ && now > sync_->deadline) {
                fail_sync(std::format("IB stopped answering for {} min, waiting for {}",
                                      kContractSyncTimeout.count(), sync_waiting_for()),
                          std::errc::timed_out);
            } else if (sync_ && now > sync_->next_log) {
                log(std::format("Syncing {}: waiting for {}", sync_->portfolio, sync_waiting_for()),
                    utilities::INFO);
                sync_->next_log = now + kContractSyncLogInterval;
            }
        }
        {
            std::scoped_lock lock(catch_up_mutex_);
            if (catch_up_ && now > catch_up_->deadline) {
                fail_catch_up(no_answer(), std::errc::timed_out);
            }
        }
        std::scoped_lock lock(account_mutex_);
        if (account_query_ && now > account_query_->deadline) {
            fail_account(no_answer(), std::errc::timed_out);
        }
    }

    /** Never hand out an order id below `id` (any thread). */
    void raise_next_order_id(int id) {
        int next = next_order_id_.load(std::memory_order_relaxed);
        while (next < id &&
               !next_order_id_.compare_exchange_weak(next, id, std::memory_order_relaxed)) {
        }
    }

    /** An order that is done: kept apart, for a fill IB reports late (message thread). */
    void forget(const std::string& order_id) {
        if (const auto it = orders_.find(order_id); it != orders_.end()) {
            completed_orders_[order_id] = std::move(it->second);
            orders_.erase(it);
        }
    }

    /** Hand over the sync's contracts once every request has answered; sync_mutex_ held. */
    void finish_sync() {
        if (!sync_->options_requested || !sync_->pending.empty() || sync_->hedge_req != 0) {
            return;
        }
        log(std::format("Synced {} contracts for {} from IB", sync_->contracts.size(),
                        sync_->portfolio),
            utilities::INFO);
        sync_->done.set_value(std::move(sync_->contracts));
        sync_.reset();
    }

    /** What the running sync is waiting for, for an error; sync_mutex_ held. */
    auto sync_waiting_for() const -> std::string {
        if (sync_->underlying_con_id == 0) {
            return "the underlying's contract";
        }
        if (!sync_->options_requested) {
            return "the option expiries";
        }
        if (!sync_->pending.empty()) {
            return std::format("the contracts of {} of {} expiries ({} contracts so far)",
                               sync_->pending.size(), sync_->expiries.size(),
                               sync_->contracts.size());
        }
        return "the hedge instrument's contract";
    }

    /** Fail the running sync, if any; sync_mutex_ held. */
    void fail_sync(const std::string& reason, std::optional<std::errc> code = std::nullopt) {
        if (!sync_) {
            return;
        }
        const std::string what = std::format("Contract sync for {}: {}", sync_->portfolio, reason);
        log(what, utilities::ERROR);
        sync_->done.set_exception(request_failure(what, code));
        sync_.reset();
    }

    auto next_request_id() -> int { return next_request_id_++; }

    /** Log an order's new state and pass it on (never under mutex_). */
    /** One order to IB (message thread). One contract at ratio 1 is its own order, at its leg's
     * side and a price paid or received (IB does not answer a BAG of one leg). More is a BAG
     * bought, its legs carrying the sides and its price the net (negative: a credit). */
    void place(const model::OrderRequest& req, model::Order od, int ib_id) {
        orders_[od.order_id] = od;
        const bool combo = req.is_combo();
        const model::Leg& leg = req.legs.front();
        const Contract contract =
            combo ? ib_combo_contract(req.legs, req.trading_class) : ib_contract(leg.symbol);
        Order order;
        order.action = combo ? "BUY" : direction_to_ib(leg.side);
        order.orderType = ordertype_to_ib(req.type);
        order.totalQuantity =
            DecimalFunctions::stringToDecimal(std::to_string(static_cast<long long>(req.quantity)));
        if (req.type == model::OrderType::LIMIT) {
            order.lmtPrice = combo ? req.price : std::abs(req.price);
        }
        order.tif = "DAY"; // IB rejects an order with none (10052); strategies trade the day
        order.orderId = ib_id;
        order.clientId = client_id_; // set by connect() before the session started
        order.account = account_;
        log(std::format("IB placing order: id={} symbol={} vol={}", od.order_id,
                        combo ? std::format("COMBO({})", contract.symbol) : req.symbol,
                        req.quantity),
            utilities::INFO);
        if (!send([&](EClientSocket& s) { s.placeOrder(order.orderId, contract, order); })) {
            log("IB send_order failed: the connection dropped", utilities::ERROR);
            od.status = model::Status::REJECTED;
            forget(od.order_id);
            emit_order(od);
        }
    }

    void emit_order(const model::Order& order) const {
        log(std::format("Order {} symbol={} quantity={} filled={} status={}", order.order_id,
                        order.symbol, order.quantity, order.filled,
                        model::to_string(order.status)),
            utilities::INFO);
        gateway_.put_order(order);
    }

    void emit_trade(const model::Fill& fill) const {
        log(std::format("Trade {} symbol={} vol={} price={}", fill.fill_id, fill.symbol,
                        fill.quantity, fill.price),
            utilities::INFO);
        gateway_.put_fill(fill);
    }

    const core::Gateway& gateway_;

    std::mutex mutex_; // the account, and the accounts IB names (connect() waits for them)
    int client_id_ = 0;
    std::string account_;                                  // IB_ACCOUNT
    std::optional<std::vector<std::string>> managed_accounts_; // the login's, once IB names them
    std::condition_variable accounts_named_;
    std::atomic<bool> account_managed_{false}; // IB_ACCOUNT among them: orders may go
    std::atomic<int> next_order_id_{0};
    // The orders: the message thread's alone (send_order and restore_orders hand theirs over).
    std::unordered_map<std::string, model::Order> orders_;
    std::unordered_map<std::string, model::Order> completed_orders_;
    std::unordered_set<std::string> unconfirmed_; // working before a restart or a drop, unchecked

    std::atomic<int> next_request_id_{kFirstRequestId};
    std::mutex sync_mutex_;
    std::optional<Sync> sync_;
    std::mutex catch_up_mutex_;
    std::optional<CatchUp> catch_up_;
    std::mutex account_mutex_;
    std::optional<AccountQuery> account_query_;
};

// IbGateway

IbGateway::IbGateway(utilities::Logger* logger)
    : core::Gateway(logger, "IBGateway"), setting_(IbSettings::from_env(log_)),
      tws_(std::make_unique<IbTws>(*this)) {}

IbGateway::~IbGateway() = default;

void IbGateway::connect() {
    if (tws_->is_connected()) {
        return;
    }
    const IbSettings& s = setting_;
    // One account, named, and a paper one unless root has named a live one: never trade an
    // account by accident.
    if (s.account.empty()) {
        throw std::runtime_error("Set IB_ACCOUNT to the account to fill (a paper account, DU...):"
                                 " the engine fills that one alone");
    }
    if (!s.is_account_allowed()) {
        throw std::runtime_error(std::format("IB_ACCOUNT is not a paper account (DU...): a live"
                                             " one is traded only once root names it in {}",
                                             s.live_account_file));
    }
    log_.write_log(std::format("Connecting to {}:{} as client {}", s.host, s.port, s.client_id),
                   utilities::INFO);
    if (!tws_->connect(s)) {
        throw std::runtime_error(std::format("Cannot reach IB at {}:{} as client {}: is IB Gateway"
                                             " running, with its API on that port?",
                                             s.host, s.port, s.client_id));
    }
    log_.write_log("IB connected", utilities::INFO);
}

void IbGateway::disconnect() { tws_->disconnect(); }

auto IbGateway::is_connected() const -> bool { return tws_->is_connected(); }

auto IbGateway::send_order(const model::OrderRequest& req) -> std::string {
    return tws_->send_order(req);
}

void IbGateway::cancel_order(const model::CancelRequest& req) { tws_->cancel_order(req); }

auto IbGateway::query_account() -> std::future<core::BrokerAccount> {
    return tws_->query_account();
}

auto IbGateway::sync_contracts(std::string_view portfolio, int max_dte)
    -> std::future<std::vector<model::Contract>> {
    const model::PortfolioSpec* p = model::find_portfolio(portfolio);
    if (p == nullptr) {
        throw std::invalid_argument(
            std::format("No portfolio {} (model/portfolio_spec.hpp)", portfolio));
    }
    // Each spec's are listed (ib_api.hpp).
    const IbListing& listing = *find_listing(p->underlying);
    const IbListing* hedge = p->hedge == p->underlying ? nullptr : find_listing(p->hedge);
    if (max_dte < 0) {
        throw std::invalid_argument("max_dte must be 0 or more");
    }
    if (!is_connected()) {
        throw std::runtime_error("Connect the IB gateway first");
    }
    return tws_->sync_contracts(p->name, listing, hedge, max_dte);
}

void IbGateway::restore_orders(std::span<const model::Order> orders) {
    tws_->restore_orders(orders);
}

auto IbGateway::catch_up() -> std::future<void> { return tws_->catch_up(); }

} // namespace adapters
