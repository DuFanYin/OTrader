#include "engine_event.hpp"
#include "../../core/engine_execution.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../strategy/template.hpp"
#include "engine_main.hpp"
#include <chrono>
#include <variant>

namespace backtest {

auto EventEngine::put_intent_send_order(const utilities::OrderRequest& req) -> std::string {
    if (main_engine_ != nullptr) {
        return main_engine_->send_order(req);
    }
    return {};
}

void EventEngine::put_intent_cancel_order(const utilities::CancelRequest& req) {
    if (main_engine_ != nullptr) {
        main_engine_->cancel_order(req);
    }
}

void EventEngine::put_intent_log(const utilities::LogData& log) {
    if (main_engine_ != nullptr) {
        main_engine_->put_log_intent(log);
    }
}

void EventEngine::put_event(const utilities::Event& event) {
    if (main_engine_ == nullptr) {
        return;
    }
    std::vector<utilities::OrderRequest> orders;
    std::vector<utilities::CancelRequest> cancels;
    std::vector<utilities::LogData> logs;
    switch (event.type) {
    case utilities::EventType::Snapshot:
        dispatch_snapshot(event);
        break;
    case utilities::EventType::Timer: {
        dispatch_timer(&orders, &cancels, &logs);
        for (const auto& o : orders) {
            main_engine_->send_order(o);
        }
        for (const auto& c : cancels) {
            main_engine_->cancel_order(c);
        }
        for (const auto& l : logs) {
            main_engine_->put_log_intent(l);
        }
        break;
    }
    case utilities::EventType::Order:
        dispatch_order(event);
        break;
    case utilities::EventType::Trade:
        dispatch_trade(event);
        break;
    default:
        break;
    }
}

void EventEngine::dispatch_snapshot(const utilities::Event& event) {
    if (const auto* snap = std::get_if<utilities::PortfolioSnapshot>(&event.data)) {
        utilities::PortfolioData* portfolio = main_engine_->get_portfolio(snap->portfolio_name);
        if (portfolio != nullptr) {
            portfolio->apply_frame(*snap);
        }
    }
}

void EventEngine::dispatch_timer(std::vector<utilities::OrderRequest>* out_orders,
                                 std::vector<utilities::CancelRequest>* out_cancels,
                                 std::vector<utilities::LogData>* out_logs) {
    core::OptionStrategyEngine* se = main_engine_->option_strategy_engine();
    if ((se == nullptr) || (se->get_strategy() == nullptr)) {
        return;
    }
    se->get_strategy()->on_timer();
    engines::PositionEngine* pos = main_engine_->position_engine();
    if ((pos != nullptr) && (se->get_strategy() != nullptr)) {
        auto* portfolio = main_engine_->get_portfolio(se->get_strategy()->portfolio_name());
        if (portfolio != nullptr) {
            pos->update_metrics(se->get_strategy()->strategy_name(), portfolio);
        }
    }
    engines::HedgeEngine* hedge = main_engine_->hedge_engine();
    if ((hedge != nullptr) && (se->get_strategy() != nullptr) &&
        ((out_orders != nullptr) || (out_cancels != nullptr) || (out_logs != nullptr))) {
        engines::HedgeParams params;
        params.portfolio = main_engine_->get_portfolio(se->get_strategy()->portfolio_name());
        params.holding = main_engine_->get_holding(se->get_strategy()->strategy_name());
        params.get_contract = [this](const std::string& sym) -> const utilities::ContractData* {
            return main_engine_->get_contract(sym);
        };
        params.get_strategy_active_orders =
            [se]() -> const std::unordered_map<std::string, std::set<std::string>>& {
            return se->get_strategy_active_orders();
        };
        params.get_order = [se](const std::string& oid) -> utilities::OrderData* {
            return se->get_order(oid);
        };
        hedge->process_hedging(se->get_strategy()->strategy_name(), params, out_orders, out_cancels,
                               out_logs);
    }
}

void EventEngine::dispatch_order(const utilities::Event& event) {
    if (const auto* p = std::get_if<utilities::OrderData>(&event.data)) {
        utilities::OrderData order = *p;
        core::ExecutionEngine* ex = main_engine_->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            strategy_name = ex->get_strategy_name_for_order(order.orderid);
            // 回测同步派发：Order 事件在 send_impl_ 返回前就派发，此时尚未 register_active_order，
            // 用单策略回退
            if (strategy_name.empty()) {
                core::OptionStrategyEngine* se = main_engine_->option_strategy_engine();
                if (se != nullptr && se->get_strategy() != nullptr) {
                    strategy_name = se->get_strategy()->strategy_name();
                }
            }
            ex->store_order(strategy_name, order);
        }
        if (main_engine_->position_engine() != nullptr) {
            main_engine_->position_engine()->process_order(order);
        }
        if (main_engine_->option_strategy_engine() != nullptr) {
            main_engine_->option_strategy_engine()->process_order(order);
        }
    }
}

void EventEngine::dispatch_trade(const utilities::Event& event) {
    if (const auto* p = std::get_if<utilities::TradeData>(&event.data)) {
        utilities::TradeData trade = *p;
        core::ExecutionEngine* ex = main_engine_->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            ex->store_trade(trade);
            strategy_name = ex->get_strategy_name_for_order(trade.orderid);
            if (strategy_name.empty()) {
                core::OptionStrategyEngine* se = main_engine_->option_strategy_engine();
                if (se != nullptr && se->get_strategy() != nullptr) {
                    strategy_name = se->get_strategy()->strategy_name();
                }
            }
        }
        if (main_engine_->position_engine() != nullptr) {
            main_engine_->position_engine()->process_trade(strategy_name, trade);
        }
        if (main_engine_->option_strategy_engine() != nullptr) {
            main_engine_->option_strategy_engine()->process_trade(trade);
        }
    }
}

} // namespace backtest
