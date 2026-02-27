#include "engine_main.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../strategy/template.hpp"
#include "../../utilities/utility.hpp"
#include "engine_data_historical.hpp"
#include <stdexcept>

namespace backtest {

namespace {
struct NoOpEventEngine : utilities::IEventEngine {};
NoOpEventEngine g_noop_event;
} // namespace

MainEngine::MainEngine(utilities::IEventEngine* event_engine) {
    if (event_engine == nullptr) {
        event_engine = &g_noop_event;
    }
    event_engine_ = event_engine;
    event_engine_->start();
    position_engine_ = std::make_unique<engines::PositionEngine>();
    log_engine_ = std::make_unique<engines::LogEngine>(this);
    execution_engine_ = std::make_unique<core::ExecutionEngine>();
    execution_engine_->set_send_impl(
        [this](const utilities::OrderRequest& req) -> std::string { return append_order(req); });
    log_engine_->set_level(
        engines::DISABLED); // no log by default; call set_log_level(engines::INFO) etc. to enable
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
    // backtest 暂不需要对外推送策略事件，这里留空实现占位
    api.system.put_strategy_event = [](const utilities::StrategyUpdateData&) -> void {};
    api.system.get_combo_builder_engine = [this]() -> engines::ComboBuilderEngine* {
        return combo_builder_engine();
    };
    api.system.get_hedge_engine = [this]() -> engines::HedgeEngine* { return hedge_engine(); };

    option_strategy_engine_ = std::make_unique<core::OptionStrategyEngine>(std::move(api));
    put_log_intent("Main engine initialization successful", INFO);
}

MainEngine::~MainEngine() = default;

void MainEngine::register_portfolio(utilities::PortfolioData* portfolio) {
    if (portfolio != nullptr) {
        portfolios_[portfolio->name] = portfolio;
    }
}

auto MainEngine::get_portfolio(const std::string& portfolio_name) const
    -> utilities::PortfolioData* {
    auto it = portfolios_.find(portfolio_name);
    return (it != portfolios_.end()) ? it->second : nullptr;
}

void MainEngine::register_contract(utilities::ContractData contract) {
    contracts_[contract.symbol] = std::move(contract);
}

auto MainEngine::get_contract(const std::string& symbol) const -> const utilities::ContractData* {
    auto it = contracts_.find(symbol);
    return (it != contracts_.end()) ? &it->second : nullptr;
}

auto MainEngine::load_backtest_data(const std::string& parquet_path,
                                    const std::string& underlying_symbol) -> BacktestDataEngine* {
    if (!data_engine_) {
        data_engine_ = std::make_unique<BacktestDataEngine>(this);
    }
    data_engine_->load_parquet(parquet_path, "ts_recv", underlying_symbol);
    put_log_intent("Backtest data loaded from: " + parquet_path, INFO);
    return data_engine_.get();
}

void MainEngine::put_event(const utilities::Event& e) {
    if (event_engine_ != nullptr) {
        event_engine_->put_event(e);
    }
}

auto MainEngine::send_order(const utilities::OrderRequest& req) -> std::string {
    if (!order_executor_) {
        throw std::runtime_error(
            "No order executor set. Use BacktestEngine for backtest execution.");
    }
    return order_executor_(req);
}

void MainEngine::add_order(std::string orderid, utilities::OrderData order) {
    order.orderid = std::move(orderid);
    if (execution_engine_) {
        execution_engine_->add_order(order);
    }
}

void MainEngine::cancel_order(const utilities::CancelRequest& req) {
    if (execution_engine_) {
        execution_engine_->remove_order_tracking(req.orderid);
        utilities::OrderData* o = execution_engine_->get_order(req.orderid);
        if (o != nullptr) {
            o->status = utilities::Status::CANCELLED;
            put_event(utilities::Event(utilities::EventType::Order, *o));
        }
    }
}

auto MainEngine::get_order(const std::string& orderid) const -> utilities::OrderData* {
    return execution_engine_ ? execution_engine_->get_order(orderid) : nullptr;
}

auto MainEngine::get_trade(const std::string& tradeid) const -> utilities::TradeData* {
    return execution_engine_ ? execution_engine_->get_trade(tradeid) : nullptr;
}

auto MainEngine::get_all_orders() const -> std::vector<utilities::OrderData> {
    return option_strategy_engine_ ? option_strategy_engine_->get_all_orders()
                                   : std::vector<utilities::OrderData>{};
}

auto MainEngine::get_all_trades() const -> std::vector<utilities::TradeData> {
    return option_strategy_engine_ ? option_strategy_engine_->get_all_trades()
                                   : std::vector<utilities::TradeData>{};
}

auto MainEngine::get_all_active_orders() const -> std::vector<utilities::OrderData> {
    return option_strategy_engine_ ? option_strategy_engine_->get_all_active_orders()
                                   : std::vector<utilities::OrderData>{};
}

void MainEngine::put_log_intent(const std::string& msg, int level) const {
    utilities::LogData intent;
    intent.msg = msg;
    intent.level = level;
    intent.gateway_name = "Main";
    put_log_intent(intent);
}

void MainEngine::put_log_intent(const utilities::LogData& intent) const {
    if (!log_engine_) {
        return;
    }
    log_engine_->process_log_intent(intent);
}

void MainEngine::set_log_level(int level) {
    if (log_engine_) {
        log_engine_->set_level(level);
    }
}

auto MainEngine::log_level() const -> int {
    return log_engine_ ? log_engine_->level() : engines::DISABLED;
}

void MainEngine::close() {
    if (option_strategy_engine_) {
        option_strategy_engine_->close();
    }
    if (execution_engine_) {
        execution_engine_->clear();
    }
    if (event_engine_ != nullptr) {
        event_engine_->stop();
    }
}

auto MainEngine::combo_builder_engine() -> engines::ComboBuilderEngine* {
    if (!combo_builder_engine_) {
        combo_builder_engine_ = std::make_unique<engines::ComboBuilderEngine>();
    }
    return combo_builder_engine_.get();
}

auto MainEngine::hedge_engine() -> engines::HedgeEngine* {
    if (!hedge_engine_) {
        hedge_engine_ = std::make_unique<engines::HedgeEngine>();
    }
    return hedge_engine_.get();
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

void MainEngine::append_log(const utilities::LogData& log) const { put_log_intent(log); }

} // namespace backtest
