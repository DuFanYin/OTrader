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
    market_data_client_ = std::make_unique<MarketDataClient>(this);
    gateway_client_ = std::make_unique<GatewayClient>(this);

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

    portfolio_structure_->ensure_portfolios_created();
    db_engine_->load_contracts(
        [this](const utilities::ContractData& c) { portfolio_structure_->process_option(c); },
        [this](const utilities::ContractData& c) { portfolio_structure_->process_underlying(c); });
    portfolio_structure_->finalize_all_chains();

    log_self_check();
    write_log("Main engine initialization successful", INFO);
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
    return gateway_client_->send_order(req);
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
    if (market_data_client_) {
        market_data_client_->close();
    }
    if (gateway_client_) {
        gateway_client_->close();
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
    if (market_data_client_ == nullptr) {
        throw std::runtime_error("market data client is null");
    }
    market_data_client_->start();
    market_data_running_ = true;
}

void MainEngine::stop_market_data_update() {
    market_data_running_ = false;
    if (market_data_client_) {
        market_data_client_->stop();
    }
}

void MainEngine::subscribe_chains(const std::string& strategy_name,
                                  std::span<const std::string> chain_symbols) {
    if (market_data_client_) {
        market_data_client_->subscribe_chains(strategy_name, chain_symbols);
    }
}

void MainEngine::unsubscribe_chains(const std::string& strategy_name) {
    if (market_data_client_) {
        market_data_client_->unsubscribe_chains(strategy_name);
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

void MainEngine::connect() { gateway_client_->connect(); }

void MainEngine::disconnect() { gateway_client_->disconnect(); }

void MainEngine::cancel_order(const utilities::CancelRequest& req) {
    if (execution_engine_) {
        execution_engine_->cancel_order(req);
    }
}

auto MainEngine::send_order(const utilities::OrderRequest& req) -> std::string {
    return gateway_client_->send_order(req);
}

auto MainEngine::send_order(const std::string& strategy_name, const utilities::OrderRequest& req)
    -> std::string {
    auto o = event_engine_
                 ? event_engine_->put_intent(utilities::IntentSendOrder{strategy_name, req})
                 : std::nullopt;
    return o.value_or("");
}

void MainEngine::query_account() { gateway_client_->query_account(); }

void MainEngine::query_position() { gateway_client_->query_position(); }

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
