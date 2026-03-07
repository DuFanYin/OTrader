#include "engine_event.hpp"
#include "../../core/engine_execution.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../strategy/template.hpp"
#include "../../utilities/intent.hpp"
#include "engine_main.hpp"
#include <chrono>
#include <variant>

namespace backtest {

auto EventEngine::put_intent(const utilities::Intent& intent) -> std::optional<std::string> {
    auto* main = static_cast<MainEngine*>(main_engine);
    switch (static_cast<utilities::IntentType>(intent.index())) {
        using enum utilities::IntentType;
    case SendOrder: {
        const auto& arg = std::get<utilities::IntentSendOrder>(intent);
        if (main != nullptr && main->execution_engine() != nullptr) {
            return main->execution_engine()->send_order(arg.strategy_name, arg.req);
        }
        return std::nullopt;
    }
    case CancelOrder: {
        const auto& arg = std::get<utilities::IntentCancelOrder>(intent);
        if (main != nullptr && main->execution_engine() != nullptr) {
            main->execution_engine()->cancel_order(arg.req);
        }
        return std::nullopt;
    }
    case Log: {
        const auto& arg = std::get<utilities::IntentLog>(intent);
        if (main != nullptr) {
            main->put_log_intent(arg.log);
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

void EventEngine::put_event(const utilities::Event& event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    std::vector<utilities::OrderRequest> orders;
    std::vector<utilities::CancelRequest> cancels;
    std::vector<utilities::LogData> logs;
    switch (event.type) {
        using enum utilities::EventType;
    case Snapshot:
        dispatch_snapshot(event);
        break;
    case Timer: {
        dispatch_timer(&orders, &cancels, &logs);
        core::OptionStrategyEngine* se = main->option_strategy_engine();
        std::string strategy_name = (se != nullptr && se->get_strategy() != nullptr)
                                        ? se->get_strategy()->strategy_name()
                                        : std::string{};
        for (const auto& o : orders) {
            put_intent(utilities::IntentSendOrder{strategy_name, o});
        }
        for (const auto& c : cancels) {
            put_intent(utilities::IntentCancelOrder{c});
        }
        for (const auto& l : logs) {
            put_intent(utilities::IntentLog{l});
        }
        break;
    }
    case Order:
        dispatch_order(event);
        break;
    case Trade:
        dispatch_trade(event);
        break;
    default:
        break;
    }
}

void EventEngine::dispatch_snapshot(const utilities::Event& event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    if (const auto* snap = std::get_if<utilities::PortfolioSnapshot>(&event.data)) {
        utilities::PortfolioData* portfolio = main->get_portfolio(snap->portfolio_name);
        if (portfolio != nullptr) {
            portfolio->apply_frame(*snap);
        }
    }
}

void EventEngine::dispatch_timer(std::vector<utilities::OrderRequest>* out_orders,
                                 std::vector<utilities::CancelRequest>* out_cancels,
                                 std::vector<utilities::LogData>* out_logs) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    core::OptionStrategyEngine* se = main->option_strategy_engine();
    if ((se == nullptr) || (se->get_strategy() == nullptr)) {
        return;
    }
    se->get_strategy()->on_timer();
    engines::PositionEngine* pos = main->position_engine();
    if ((pos != nullptr) && (se->get_strategy() != nullptr)) {
        auto* portfolio = main->get_portfolio(se->get_strategy()->portfolio_name());
        if (portfolio != nullptr) {
            pos->update_metrics(se->get_strategy()->strategy_name(), portfolio);
        }
    }
    engines::HedgeEngine* hedge = main->hedge_engine();
    if ((hedge != nullptr) && (se->get_strategy() != nullptr) &&
        ((out_orders != nullptr) || (out_cancels != nullptr) || (out_logs != nullptr))) {
        engines::HedgeParams params;
        params.portfolio = main->get_portfolio(se->get_strategy()->portfolio_name());
        params.holding = main->get_holding(se->get_strategy()->strategy_name());
        params.get_contract = [main](const std::string& sym) -> const utilities::ContractData* {
            return main->get_contract(sym);
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
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    if (const auto* p = std::get_if<utilities::OrderData>(&event.data)) {
        utilities::OrderData order = *p;
        core::ExecutionEngine* ex = main->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            strategy_name = ex->get_strategy_name_for_order(order.orderid);
            // Backtest: Order before register_active_order; fallback to single strategy
            if (strategy_name.empty()) {
                core::OptionStrategyEngine* se = main->option_strategy_engine();
                if (se != nullptr && se->get_strategy() != nullptr) {
                    strategy_name = se->get_strategy()->strategy_name();
                }
            }
            ex->store_order(strategy_name, order);
        }
        if (main->position_engine() != nullptr) {
            main->position_engine()->process_order(strategy_name, order);
        }
        if (main->option_strategy_engine() != nullptr) {
            main->option_strategy_engine()->process_order(order);
        }
    }
}

void EventEngine::dispatch_trade(const utilities::Event& event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    if (const auto* p = std::get_if<utilities::TradeData>(&event.data)) {
        utilities::TradeData trade = *p;
        core::ExecutionEngine* ex = main->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            ex->store_trade(trade);
            strategy_name = ex->get_strategy_name_for_order(trade.orderid);
            if (strategy_name.empty()) {
                core::OptionStrategyEngine* se = main->option_strategy_engine();
                if (se != nullptr && se->get_strategy() != nullptr) {
                    strategy_name = se->get_strategy()->strategy_name();
                }
            }
        }
        if (main->position_engine() != nullptr) {
            main->position_engine()->process_trade(strategy_name, trade);
        }
        if (main->option_strategy_engine() != nullptr) {
            main->option_strategy_engine()->process_trade(trade);
        }
    }
}

} // namespace backtest
