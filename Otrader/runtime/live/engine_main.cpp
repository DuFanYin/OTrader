/** Live MainEngine. */

#include "engine_main.hpp"
#include "../../strategy/strategy_registry.hpp"
#include "../../utilities/intent.hpp"
#include "engine_event.hpp"
#include <chrono>
#include <format>
#include <optional>
#include <utility>

namespace engines {

MainEngine::MainEngine() {
    event_engine_ = std::make_unique<EventEngine>(this, 1);
    event_engine_->start();
    db_engine_ = std::make_unique<DatabaseEngine>(this);
    market_data_ = std::make_unique<MarketDataEngine>(this);
    gateway_ = std::make_unique<LiveGateway>(this);

    // In-process data plane: market-data snapshots and gateway order/trade events feed the
    // event engine directly (acquire a pooled slot, copy in, put_event) — the same work the old
    // ZMQ SUB threads did after deserialization, minus the serialization + IPC hops.
    market_data_->set_snapshot_callback([this](const utilities::PortfolioSnapshot& s) {
        utilities::PortfolioSnapshot* p = acquire_snapshot();
        if (p != nullptr) {
            *p = s;
            put_event(utilities::Event(utilities::EventType::Snapshot, p));
        }
    });
    gateway_->set_order_callback([this](const utilities::OrderData& o) {
        utilities::OrderData* p = acquire_order();
        if (p != nullptr) {
            *p = o;
            put_event(utilities::Event(utilities::EventType::Order, p));
        }
    });
    gateway_->set_trade_callback([this](const utilities::TradeData& t) {
        utilities::TradeData* p = acquire_trade();
        if (p != nullptr) {
            *p = t;
            put_event(utilities::Event(utilities::EventType::Trade, p));
        }
    });

    init_core(event_engine_.get(),
              CoreInitParams{
                  .send_impl = [this](const utilities::OrderRequest& req) -> std::string {
                      return send_order(req);
                  },
                  .cancel_impl = [this](core::ExecutionEngine*,
                                        const utilities::CancelRequest& req) { cancel_order(req); },
                  .put_strategy_event =
                      [this](const utilities::StrategyUpdateData& u) { on_strategy_event(u); },
                  .log_level = engines::INFO,
              });

    // Engine's own portfolio view (snapshot consumer / apply_frame).
    portfolio_structure_->ensure_portfolios_created();
    db_engine_->load_contracts(
        [this](const utilities::ContractData& c) { portfolio_structure_->process_option(c); },
        [this](const utilities::ContractData& c) { portfolio_structure_->process_underlying(c); });
    portfolio_structure_->finalize_all_chains();

    // Market-data engine is itself a PortfolioStructure (snapshot producer); load its chains too.
    market_data_->ensure_portfolios_created();
    db_engine_->load_contracts(
        [this](const utilities::ContractData& c) { market_data_->process_option(c); },
        [this](const utilities::ContractData& c) { market_data_->process_underlying(c); });
    market_data_->finalize_all_chains();

    log_self_check();
    write_log("Main engine initialization successful", INFO);
}

// Drives the gateway's periodic housekeeping (IB TWS message-queue drain; no-op for NullGateway).
// Replaces the old gateway process's 200ms timer thread now that the gateway is in-process.
void MainEngine::run_gateway_pump(const std::stop_token& st) {
    using namespace std::chrono_literals;
    while (!st.stop_requested()) {
        gateway_->process_timer_event(utilities::Event(utilities::EventType::Timer));
        std::this_thread::sleep_for(200ms);
    }
}

MainEngine::~MainEngine() { close(); }

void MainEngine::log_self_check() {
    auto classes = strategy_cpp::StrategyRegistry::get_all_strategy_class_names();
    write_log(std::format("Registered strategy classes: {}", classes.size()), INFO);
    for (const std::string& name : get_all_portfolio_names()) {
        utilities::PortfolioData* p = get_portfolio(name);
        if (p == nullptr) {
            continue;
        }
        std::string underlying_str = (p->underlying != nullptr) ? p->underlying->symbol : "None";
        write_log(p->name + " (underlying: " + underlying_str + ")", INFO);
        write_log("  chains: " + std::to_string(p->chains.size()), INFO);
        write_log("  options: " + std::to_string(p->option_apply_order().size()), INFO);
    }
}

// ---- Infra hooks ----

std::string MainEngine::send_order_to_gateway(const utilities::OrderRequest& req) {
    return gateway_->send_order(req);
}

void MainEngine::save_order_data(const std::string& strategy_name,
                                 const utilities::OrderData& order) {
    db_engine_->save_order_data(strategy_name, order);
}

void MainEngine::save_trade_data(const std::string& strategy_name,
                                 const utilities::TradeData& trade) {
    db_engine_->save_trade_data(strategy_name, trade);
}

void MainEngine::close_infra() {
    gateway_pump_thread_.request_stop();
    if (gateway_pump_thread_.joinable()) {
        gateway_pump_thread_.join();
    }
    if (market_data_) {
        market_data_->stop_market_data_update();
    }
    if (gateway_) {
        gateway_->close();
    }
    if (event_engine_) {
        event_engine_->close();
    }
    if (db_engine_) {
        db_engine_->close();
    }
}

// ---- Live-specific methods ----

void MainEngine::start_market_data_update() {
    if (market_data_ == nullptr) {
        throw std::runtime_error("market data engine is null");
    }
    market_data_->start_market_data_update();
    market_data_running_ = true;
}

void MainEngine::stop_market_data_update() {
    market_data_running_ = false;
    if (market_data_) {
        market_data_->stop_market_data_update();
    }
}

void MainEngine::subscribe_chains(const std::string& strategy_name,
                                  std::span<const std::string> chain_symbols) {
    if (market_data_) {
        market_data_->subscribe_chains(strategy_name, chain_symbols);
    }
}

void MainEngine::unsubscribe_chains(const std::string& strategy_name) {
    if (market_data_) {
        market_data_->unsubscribe_chains(strategy_name);
    }
}

auto MainEngine::get_all_portfolio_names() const -> std::vector<std::string> {
    return portfolio_structure_->get_all_portfolio_names();
}

auto MainEngine::get_all_contracts() const -> std::vector<utilities::ContractData> {
    return portfolio_structure_->get_all_contracts();
}

auto MainEngine::get_strategy_errors() const -> std::vector<std::pair<std::string, std::string>> {
    return option_strategy_engine_ ? option_strategy_engine()->get_strategy_errors()
                                   : std::vector<std::pair<std::string, std::string>>{};
}

void MainEngine::connect() {
    gateway_->connect();
    if (!gateway_pump_thread_.joinable()) {
        gateway_pump_thread_ =
            std::jthread([this](const std::stop_token& st) { run_gateway_pump(st); });
    }
}

void MainEngine::disconnect() {
    gateway_pump_thread_.request_stop();
    if (gateway_pump_thread_.joinable()) {
        gateway_pump_thread_.join();
    }
    gateway_->disconnect();
}

void MainEngine::cancel_order(const utilities::CancelRequest& req) {
    if (execution_engine_) {
        execution_engine_->cancel_order(req);
    }
}

auto MainEngine::send_order(const utilities::OrderRequest& req) -> std::string {
    return gateway_->send_order(req);
}

auto MainEngine::send_order(const std::string& strategy_name, const utilities::OrderRequest& req)
    -> std::string {
    auto o = event_engine_
                 ? event_engine_->put_intent(utilities::IntentSendOrder{strategy_name, req})
                 : std::nullopt;
    return o.value_or("");
}

void MainEngine::query_account() { gateway_->query_account(); }

void MainEngine::query_position() { gateway_->query_position(); }

auto MainEngine::get_trade(const std::string& tradeid) -> utilities::TradeData* {
    return execution_engine_ ? execution_engine_->get_trade(tradeid) : nullptr;
}

void MainEngine::on_strategy_event(const utilities::StrategyUpdateData& update) {
    utilities::StrategyUpdateData* p = strategy_updates_pool_.acquire();
    if (p != nullptr) {
        *p = update;
        if (strategy_updates_ring_.try_push(p)) {
            strategy_updates_cv_.notify_one();
        } else {
            strategy_updates_pool_.release(p);
        }
    }
}

auto MainEngine::pop_strategy_update(utilities::StrategyUpdateData& out, int timeout_ms) -> bool {
    std::unique_lock<std::mutex> lock(strategy_updates_mutex_);
    if (!strategy_updates_cv_.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [this]() -> bool { return !strategy_updates_ring_.empty(); })) {
        return false;
    }
    utilities::StrategyUpdateData* p = nullptr;
    if (!strategy_updates_ring_.try_pop(p) || p == nullptr) {
        return false;
    }
    lock.unlock();
    out = std::move(*p);
    strategy_updates_pool_.release(p);
    return true;
}

void MainEngine::put_event(const utilities::Event& e) { event_engine_->put_event(e); }

void MainEngine::put_event(utilities::Event&& e) {
    event_engine_->put_event(std::forward<utilities::Event>(e));
}

auto MainEngine::acquire_snapshot() -> utilities::PortfolioSnapshot* {
    return event_engine_ ? event_engine_->acquire_snapshot() : nullptr;
}

auto MainEngine::acquire_order() -> utilities::OrderData* {
    return event_engine_ ? event_engine_->acquire_order() : nullptr;
}

auto MainEngine::acquire_trade() -> utilities::TradeData* {
    return event_engine_ ? event_engine_->acquire_trade() : nullptr;
}

auto MainEngine::pop_log_for_stream(utilities::LogData& out, int timeout_ms) -> bool {
    return log_engine() ? log_engine()->pop_log_for_stream(out, timeout_ms) : false;
}

} // namespace engines
