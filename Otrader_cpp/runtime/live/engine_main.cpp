/**
 * C++ implementation of engines/engine_main.py
 */

#include "engine_main.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../strategy/strategy_registry.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/utility.hpp"
#include "engine_event.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace engines {

MainEngine::MainEngine(EventEngine* event_engine) {
    if (event_engine == nullptr) {
        event_engine_ = std::make_unique<EventEngine>(1);
        event_engine_ptr_ = event_engine_.get();
    } else {
        event_engine_ptr_ = event_engine;
    }
    event_engine_ptr_->start();

    log_engine_ = std::make_unique<LogEngine>(this);
    position_engine_ = std::make_unique<PositionEngine>();
    execution_engine_ = std::make_unique<core::ExecutionEngine>();
    execution_engine_->set_send_impl(
        [this](const utilities::OrderRequest& req) -> std::string { return append_order(req); });
    db_engine_ = std::make_unique<DatabaseEngine>(this);
    market_data_engine_ = std::make_unique<MarketDataEngine>(this);
    ib_gateway_ = std::make_unique<IbGateway>(this);
    core::RuntimeAPI api;

    // Execution API
    api.execution.send_order = [this](const std::string& strategy_name,
                                      const utilities::OrderRequest& req) -> std::string {
        return execution_engine_ ? execution_engine_->send_order(strategy_name, req)
                                 : std::string{};
    };
    api.execution.cancel_order = [this](const utilities::CancelRequest& req) -> void {
        cancel_order(req);
    };
    api.execution.get_order = [this](const std::string& oid) -> utilities::OrderData* {
        return execution_engine_ ? execution_engine_->get_order(oid) : nullptr;
    };
    api.execution.get_trade = [this](const std::string& tid) -> utilities::TradeData* {
        return execution_engine_ ? execution_engine_->get_trade(tid) : nullptr;
    };
    api.execution.get_strategy_name_for_order = [this](const std::string& oid) -> std::string {
        return execution_engine_ ? execution_engine_->get_strategy_name_for_order(oid)
                                 : std::string{};
    };
    api.execution.get_all_orders = [this]() -> std::vector<utilities::OrderData> {
        return execution_engine_ ? execution_engine_->get_all_orders()
                                 : std::vector<utilities::OrderData>{};
    };
    api.execution.get_all_trades = [this]() -> std::vector<utilities::TradeData> {
        return execution_engine_ ? execution_engine_->get_all_trades()
                                 : std::vector<utilities::TradeData>{};
    };
    api.execution.get_all_active_orders = [this]() -> std::vector<utilities::OrderData> {
        return execution_engine_ ? execution_engine_->get_all_active_orders()
                                 : std::vector<utilities::OrderData>{};
    };
    api.execution.get_strategy_active_orders =
        [this]() -> const std::unordered_map<std::string, std::set<std::string>>& {
        static const std::unordered_map<std::string, std::set<std::string>> empty;
        return execution_engine_ ? execution_engine_->get_strategy_active_orders() : empty;
    };
    api.execution.remove_order_tracking = [this](const std::string& oid) -> void {
        if (execution_engine_) {
            execution_engine_->remove_order_tracking(oid);
        }
    };
    api.execution.get_active_order_ids = [this]() -> std::unordered_set<std::string>& {
        return execution_engine_ ? execution_engine_->active_order_ids() : dummy_active_ids_;
    };
    api.execution.ensure_strategy_key = [this](const std::string& name) -> void {
        if (execution_engine_) {
            execution_engine_->ensure_strategy_key(name);
        }
    };
    api.execution.remove_strategy_tracking = [this](const std::string& name) -> void {
        if (execution_engine_) {
            execution_engine_->remove_strategy_tracking(name);
        }
    };

    // Portfolio API
    api.portfolio.get_portfolio = [this](const std::string& name) -> utilities::PortfolioData* {
        return get_portfolio(name);
    };
    api.portfolio.get_contract =
        [this](const std::string& symbol) -> const utilities::ContractData* {
        return get_contract(symbol);
    };
    api.portfolio.get_holding = [this](const std::string& name) -> utilities::StrategyHolding* {
        return get_holding(name);
    };
    api.portfolio.get_or_create_holding = [this](const std::string& name) -> void {
        get_or_create_holding(name);
    };
    api.portfolio.remove_strategy_holding = [this](const std::string& name) -> void {
        if (position_engine_) {
            position_engine_->remove_strategy_holding(name);
        }
    };

    // System API
    api.system.write_log = [this](const utilities::LogData& log) -> void { put_log_intent(log); };
    api.system.put_strategy_event = [this](const utilities::StrategyUpdateData& u) -> void {
        on_strategy_event(u);
    };
    api.system.get_combo_builder_engine = [this]() -> ComboBuilderEngine* {
        return combo_builder_engine();
    };
    api.system.get_hedge_engine = [this]() -> HedgeEngine* { return hedge_engine(); };

    option_strategy_engine_ = std::make_unique<core::OptionStrategyEngine>(std::move(api));
    event_engine_ptr_->set_main_engine(this);

    // Engine 先创建 portfolio，再两段 load（option → equity）仅 find；最后 finalize_all_chains
    market_data_engine_->ensure_portfolios_created();
    db_engine_->load_contracts(
        [this](const utilities::ContractData& c) -> void {
            market_data_engine_->process_option(c);
        },
        [this](const utilities::ContractData& c) -> void {
            market_data_engine_->process_underlying(c);
        });
    market_data_engine_->finalize_all_chains();

    // 自检：打印已注册策略 class 列表
    {
        std::vector<std::string> classes =
            strategy_cpp::StrategyRegistry::get_all_strategy_class_names();
        std::ostringstream os;
        os << "Registered strategy classes: " << classes.size();
        MainEngine::write_log(os.str(), INFO);
    }

    // 自检：对每个 portfolio 打印 name、chains 数、options 数
    {
        for (const std::string& name : get_all_portfolio_names()) {
            utilities::PortfolioData* p = get_portfolio(name);
            if (p == nullptr) {
                continue;
            }
            std::string underlying_str =
                (p->underlying != nullptr) ? p->underlying->symbol : "None";
            MainEngine::write_log(p->name + " (underlying: " + underlying_str + ")", INFO);
            MainEngine::write_log("  chains: " + std::to_string(p->chains.size()), INFO);
            MainEngine::write_log("  options: " + std::to_string(p->option_apply_order().size()),
                                  INFO);
        }
    }

    MainEngine::write_log("Main engine initialization successful", INFO);
}

MainEngine::~MainEngine() { close(); }

void MainEngine::start_market_data_update() {
    if (market_data_engine_ == nullptr) {
        throw std::runtime_error("market data engine is null");
    }
    market_data_engine_->start_market_data_update();
    market_data_running_ = true;
}

void MainEngine::stop_market_data_update() {
    market_data_running_ = false;
    if (market_data_engine_) {
        market_data_engine_->stop_market_data_update();
    }
}

void MainEngine::subscribe_chains(const std::string& strategy_name,
                                  const std::vector<std::string>& chain_symbols) {
    market_data_engine_->subscribe_chains(strategy_name, chain_symbols);
}

void MainEngine::unsubscribe_chains(const std::string& strategy_name) {
    market_data_engine_->unsubscribe_chains(strategy_name);
}

auto MainEngine::get_portfolio(const std::string& portfolio_name) -> utilities::PortfolioData* {
    return market_data_engine_->get_portfolio(portfolio_name);
}

auto MainEngine::get_all_portfolio_names() const -> std::vector<std::string> {
    return market_data_engine_->get_all_portfolio_names();
}

auto MainEngine::get_contract(const std::string& symbol) const -> const utilities::ContractData* {
    return market_data_engine_->get_contract(symbol);
}

auto MainEngine::get_all_contracts() const -> std::vector<utilities::ContractData> {
    return market_data_engine_->get_all_contracts();
}

void MainEngine::save_trade_data(const std::string& strategy_name,
                                 const utilities::TradeData& trade) {
    db_engine_->save_trade_data(strategy_name, trade);
}

void MainEngine::save_order_data(const std::string& strategy_name,
                                 const utilities::OrderData& order) {
    db_engine_->save_order_data(strategy_name, order);
}

void MainEngine::connect() { ib_gateway_->connect(); }

void MainEngine::disconnect() { ib_gateway_->disconnect(); }

void MainEngine::cancel_order(const utilities::CancelRequest& req) {
    if (execution_engine_) {
        execution_engine_->remove_order_tracking(req.orderid);
    }
    ib_gateway_->cancel_order(req);
}

auto MainEngine::send_order(const utilities::OrderRequest& req) -> std::string {
    return ib_gateway_->send_order(req);
}

void MainEngine::query_account() { ib_gateway_->query_account(); }

void MainEngine::query_position() { ib_gateway_->query_position(); }

auto MainEngine::get_order(const std::string& orderid) -> utilities::OrderData* {
    return execution_engine_ ? execution_engine_->get_order(orderid) : nullptr;
}

auto MainEngine::get_trade(const std::string& tradeid) -> utilities::TradeData* {
    return execution_engine_ ? execution_engine_->get_trade(tradeid) : nullptr;
}

void MainEngine::on_strategy_event(const utilities::StrategyUpdateData& update) {
    {
        std::scoped_lock lock(strategy_updates_mutex_);
        strategy_updates_.push_back(update);
        if (strategy_updates_.size() > 1000) {
            strategy_updates_.pop_front();
        }
    }
    strategy_updates_cv_.notify_one();
}

auto MainEngine::pop_strategy_update(utilities::StrategyUpdateData& out, int timeout_ms) -> bool {
    std::unique_lock<std::mutex> lock(strategy_updates_mutex_);
    if (!strategy_updates_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                       [this]() -> bool { return !strategy_updates_.empty(); })) {
        return false;
    }
    out = std::move(strategy_updates_.front());
    strategy_updates_.pop_front();
    return true;
}

void MainEngine::put_event(const utilities::Event& e) {
    if (event_engine_ptr_ != nullptr) {
        event_engine_ptr_->put_event(e);
    }
}

void MainEngine::write_log(const std::string& msg, int level, const std::string& gateway) {
    if (log_engine_) {
        log_engine_->write_log(msg, level, gateway);
    }
}

void MainEngine::put_log_intent(const utilities::LogData& log) {
    if (log_engine_) {
        log_engine_->process_log_intent(log);
    }
}

void MainEngine::close() {
    option_strategy_engine_->close();
    if (execution_engine_) {
        execution_engine_->clear();
    }
    db_engine_->close();
    ib_gateway_->disconnect();
    if (event_engine_ptr_ != nullptr) {
        event_engine_ptr_->stop();
    }
}

auto MainEngine::hedge_engine() -> HedgeEngine* {
    if (!hedge_engine_) {
        hedge_engine_ = std::make_unique<HedgeEngine>();
    }
    return hedge_engine_.get();
}

auto MainEngine::combo_builder_engine() -> ComboBuilderEngine* {
    if (!combo_builder_engine_) {
        combo_builder_engine_ = std::make_unique<ComboBuilderEngine>();
    }
    return combo_builder_engine_.get();
}

auto MainEngine::get_holding(const std::string& strategy_name) -> utilities::StrategyHolding* {
    return position_engine_ ? &position_engine_->get_holding(strategy_name) : nullptr;
}

auto MainEngine::get_holding(const std::string& strategy_name) const
    -> const utilities::StrategyHolding* {
    return const_cast<MainEngine*>(this)->get_holding(strategy_name);
}

void MainEngine::get_or_create_holding(const std::string& strategy_name) {
    if (position_engine_) {
        position_engine_->get_create_strategy_holding(strategy_name);
    }
}

auto MainEngine::append_order(const utilities::OrderRequest& req) -> std::string {
    return send_order(req);
}

void MainEngine::append_cancel(const utilities::CancelRequest& req) { cancel_order(req); }

void MainEngine::append_log(const utilities::LogData& log) { put_log_intent(log); }

void MainEngine::set_log_level(int level) {
    if (log_engine_) {
        log_engine_->set_level(level);
    }
}

auto MainEngine::log_level() const -> int { return log_engine_ ? log_engine_->level() : DISABLED; }

auto MainEngine::pop_log_for_stream(utilities::LogData& out, int timeout_ms) -> bool {
    return log_engine_ ? log_engine_->pop_log_for_stream(out, timeout_ms) : false;
}

} // namespace engines
