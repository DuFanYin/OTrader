#include "main_engine_base.hpp"

#include "../../core/runtime_api.hpp"
#include "../../utilities/intent.hpp"

#include <optional>
#include <utility>

namespace runtime_common {

MainEngineBase::MainEngineBase() = default;
MainEngineBase::~MainEngineBase() = default;

void MainEngineBase::init_core(utilities::BaseEngine* event_engine, const CoreInitParams& params) {
    portfolio_structure_ = std::make_unique<engines::PortfolioStructure>();
    log_engine_ = std::make_unique<engines::LogEngine>(this);
    log_engine_->set_level(params.log_level);
    position_engine_ = std::make_unique<engines::PositionEngine>(this);
    execution_engine_ = std::make_unique<core::ExecutionEngine>(this);

    if (params.send_impl) {
        execution_engine_->set_send_impl(params.send_impl);
    }
    if (params.cancel_impl) {
        core::ExecutionEngine* exe = execution_engine_.get();
        execution_engine_->set_cancel_impl(
            [exe, fn = params.cancel_impl](const utilities::CancelRequest& req) { fn(exe, req); });
    }

    // Build RuntimeAPI — all lambdas capture stable pointers (core engines live for the
    // lifetime of MainEngineBase).
    core::RuntimeAPI api;

    auto* ee_ptr = execution_engine_.get();
    auto* pe_ptr = position_engine_.get();

    api.execution.send_order = [this](const std::string& strategy_name,
                                      const utilities::OrderRequest& req) -> std::string {
        return send_order(strategy_name, req);
    };
    api.execution.cancel_order = [this](const utilities::CancelRequest& req) -> void {
        cancel_order(req);
    };
    api.execution.get_order = [ee_ptr](const std::string& oid) -> utilities::OrderData* {
        return ee_ptr ? ee_ptr->get_order(oid) : nullptr;
    };
    api.execution.get_trade = [ee_ptr](const std::string& tid) -> utilities::TradeData* {
        return ee_ptr ? ee_ptr->get_trade(tid) : nullptr;
    };
    api.execution.get_strategy_name_for_order = [ee_ptr](const std::string& oid) -> std::string {
        return ee_ptr ? ee_ptr->get_strategy_name_for_order(oid) : std::string{};
    };
    api.execution.get_all_orders = [ee_ptr]() -> std::vector<utilities::OrderData> {
        return ee_ptr ? ee_ptr->get_all_orders() : std::vector<utilities::OrderData>{};
    };
    api.execution.get_all_trades = [ee_ptr]() -> std::vector<utilities::TradeData> {
        return ee_ptr ? ee_ptr->get_all_trades() : std::vector<utilities::TradeData>{};
    };
    api.execution.get_all_active_orders = [ee_ptr]() -> std::vector<utilities::OrderData> {
        return ee_ptr ? ee_ptr->get_all_active_orders() : std::vector<utilities::OrderData>{};
    };
    api.execution.get_strategy_active_orders =
        [ee_ptr]() -> const std::unordered_map<std::string, std::set<std::string>>& {
        static const std::unordered_map<std::string, std::set<std::string>> empty;
        return ee_ptr ? ee_ptr->get_strategy_active_orders() : empty;
    };
    api.execution.remove_order_tracking = [ee_ptr](const std::string& oid) -> void {
        if (ee_ptr) {
            ee_ptr->remove_order_tracking(oid);
        }
    };
    api.execution.get_active_order_ids = [ee_ptr, this]() -> std::unordered_set<std::string>& {
        return ee_ptr ? ee_ptr->active_order_ids() : dummy_active_ids_;
    };
    api.execution.ensure_strategy_key = [ee_ptr](const std::string& name) -> void {
        if (ee_ptr) {
            ee_ptr->ensure_strategy_key(name);
        }
    };
    api.execution.remove_strategy_tracking = [ee_ptr](const std::string& name) -> void {
        if (ee_ptr) {
            ee_ptr->remove_strategy_tracking(name);
        }
    };

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
    api.portfolio.remove_strategy_holding = [pe_ptr](const std::string& name) -> void {
        if (pe_ptr) {
            pe_ptr->remove_strategy_holding(name);
        }
    };

    api.system.write_log = [this](const utilities::LogData& log) -> void { put_log_intent(log); };
    api.system.put_strategy_event = params.put_strategy_event
                                        ? params.put_strategy_event
                                        : [](const utilities::StrategyUpdateData&) {};
    api.system.get_hedge_engine = [this]() -> engines::HedgeEngine* { return hedge_engine(); };

    option_strategy_engine_ = std::make_unique<core::OptionStrategyEngine>(this, std::move(api));
    option_strategy_engine_->load_strategy_config();
}

// ---- Shared method implementations ----

auto MainEngineBase::get_portfolio(const std::string& name) -> utilities::PortfolioData* {
    return portfolio_structure_ ? portfolio_structure_->get_portfolio(name) : nullptr;
}

auto MainEngineBase::get_contract(const std::string& symbol) const
    -> const utilities::ContractData* {
    return portfolio_structure_ ? portfolio_structure_->get_contract(symbol) : nullptr;
}

auto MainEngineBase::get_order(const std::string& orderid) -> utilities::OrderData* {
    return execution_engine_ ? execution_engine_->get_order(orderid) : nullptr;
}

auto MainEngineBase::get_holding(const std::string& strategy_name) -> utilities::StrategyHolding* {
    return position_engine_ ? &position_engine_->get_holding(strategy_name) : nullptr;
}

auto MainEngineBase::get_holding(const std::string& strategy_name) const
    -> const utilities::StrategyHolding* {
    return const_cast<MainEngineBase*>(this)->get_holding(strategy_name);
}

void MainEngineBase::get_or_create_holding(const std::string& strategy_name) {
    if (position_engine_) {
        position_engine_->get_create_strategy_holding(strategy_name);
    }
}

auto MainEngineBase::get_strategy_names_ref() const -> const std::vector<std::string>& {
    static const std::vector<std::string> empty;
    return option_strategy_engine_ ? option_strategy_engine_->get_strategy_names_ref() : empty;
}

auto MainEngineBase::get_strategy_active_orders() const
    -> const std::unordered_map<std::string, std::set<std::string>>& {
    static const std::unordered_map<std::string, std::set<std::string>> empty;
    return execution_engine_ ? execution_engine_->get_strategy_active_orders() : empty;
}

void MainEngineBase::write_log(const std::string& msg, int level, const std::string& gateway) {
    if (!log_engine_) {
        return;
    }
    utilities::LogData log;
    log.msg = msg;
    log.level = level;
    log.gateway_name = gateway.empty() ? "Main" : gateway;
    put_log_intent(log);
}

void MainEngineBase::put_log_intent(const utilities::LogData& log) {
    if (log_engine_) {
        log_engine_->process_log_intent(log);
    }
}

auto MainEngineBase::acquire_log() -> utilities::LogData* {
    return log_engine_ ? log_engine_->acquire_log() : nullptr;
}

void MainEngineBase::release_log(utilities::LogData* p) {
    if (log_engine_ && p) {
        log_engine_->release_log(p);
    }
}

void MainEngineBase::set_log_level(int level) {
    if (log_engine_) {
        log_engine_->set_level(level);
    }
}

auto MainEngineBase::log_level() const -> int {
    return log_engine_ ? log_engine_->level() : engines::DISABLED;
}

auto MainEngineBase::hedge_engine() -> engines::HedgeEngine* {
    if (!hedge_engine_) {
        hedge_engine_ = std::make_unique<engines::HedgeEngine>(this);
        core::RuntimeAPI api;

        auto* ee_ptr = execution_engine_.get();

        api.execution.send_order = [this](const std::string& strategy_name,
                                          const utilities::OrderRequest& req) -> std::string {
            return send_order(strategy_name, req);
        };
        api.execution.cancel_order = [this](const utilities::CancelRequest& req) -> void {
            cancel_order(req);
        };
        api.execution.get_order = [ee_ptr](const std::string& oid) -> utilities::OrderData* {
            return ee_ptr ? ee_ptr->get_order(oid) : nullptr;
        };
        api.execution.get_strategy_active_orders =
            [ee_ptr]() -> const std::unordered_map<std::string, std::set<std::string>>& {
            static const std::unordered_map<std::string, std::set<std::string>> empty;
            return ee_ptr ? ee_ptr->get_strategy_active_orders() : empty;
        };

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

        api.system.write_log = [this](const utilities::LogData& log) -> void { put_log_intent(log); };
        api.system.get_strategy_names_ref = [this]() -> const std::vector<std::string>& {
            return get_strategy_names_ref();
        };
        hedge_engine_->set_runtime_api(std::move(api));
    }
    return hedge_engine_.get();
}

void MainEngineBase::close() {
    close_infra();
    if (option_strategy_engine_) {
        option_strategy_engine_->close();
    }
    if (execution_engine_) {
        execution_engine_->close();
    }
}

} // namespace runtime_common
