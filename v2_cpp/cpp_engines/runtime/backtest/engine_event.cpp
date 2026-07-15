#include "engine_event.hpp"
#include "../../core/engine_execution.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../strategy/template.hpp"
#include "../../utilities/intent.hpp"
#include "engine_main.hpp"
#include <chrono>
#include <utility>
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

void EventEngine::run_dispatch(utilities::Event* p) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    std::vector<utilities::OrderRequest> orders;
    std::vector<utilities::CancelRequest> cancels;
    std::vector<utilities::LogData*> logs;
    switch (p->type) {
        using enum utilities::EventType;
    case Snapshot:
        dispatch_snapshot(*p);
        break;
    case Timer: {
        dispatch_timer(&orders, &cancels, &logs, [main]() { return main->acquire_log(); });
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
        for (utilities::LogData* ptr : logs) {
            put_intent(utilities::IntentLog{*ptr});
            main->release_log(ptr);
        }
        break;
    }
    case Order:
        dispatch_order(*p);
        break;
    case Trade:
        dispatch_trade(*p);
        break;
    default:
        break;
    }
}

void EventEngine::release_event_payload(utilities::Event* e) {
    if (e == nullptr) {
        return;
    }
    if (e->type == utilities::EventType::Snapshot) {
        if (const auto* slot = std::get_if<utilities::PortfolioSnapshot*>(&e->data)) {
            if (*slot != nullptr) {
                release_snapshot(*slot);
            }
        }
    } else if (e->type == utilities::EventType::Order) {
        if (const auto* slot = std::get_if<utilities::OrderData*>(&e->data)) {
            if (*slot != nullptr) {
                release_order(*slot);
            }
        }
    } else if (e->type == utilities::EventType::Trade) {
        if (const auto* slot = std::get_if<utilities::TradeData*>(&e->data)) {
            if (*slot != nullptr) {
                release_trade(*slot);
            }
        }
    }
}

void EventEngine::put_event(const utilities::Event& event) {
    if (main_engine == nullptr) {
        return;
    }
    utilities::Event* p = event_pool_.acquire();
    *p = event;
    run_dispatch(p);
    release_event_payload(p);
    event_pool_.release(p);
}

void EventEngine::put_event(utilities::Event&& event) {
    if (main_engine == nullptr) {
        return;
    }
    utilities::Event* p = event_pool_.acquire();
    *p = event;
    run_dispatch(p);
    release_event_payload(p);
    event_pool_.release(p);
}

void EventEngine::dispatch_snapshot(const utilities::Event& event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    if (const auto* slot = std::get_if<utilities::PortfolioSnapshot*>(&event.data)) {
        utilities::PortfolioSnapshot* snap = *slot;
        if (snap != nullptr) {
            utilities::PortfolioData* portfolio = main->get_portfolio(snap->portfolio_name);
            if (portfolio != nullptr) {
                portfolio->apply_frame(*snap);
            }
            // Do not release here; release_event_payload() releases after run_dispatch.
        }
    }
}

void EventEngine::dispatch_timer(std::vector<utilities::OrderRequest>* out_orders,
                                 std::vector<utilities::CancelRequest>* out_cancels,
                                 std::vector<utilities::LogData*>* out_logs,
                                 const std::function<utilities::LogData*()>& acquire_log) {
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
                               out_logs, acquire_log);
    }
}

void EventEngine::dispatch_order(const utilities::Event& event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    if (const auto* slot = std::get_if<utilities::OrderData*>(&event.data)) {
        utilities::OrderData* ord = *slot;
        if (ord == nullptr) {
            return;
        }
        core::ExecutionEngine* ex = main->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            strategy_name = ex->get_strategy_name_for_order(ord->orderid);
            if (strategy_name.empty()) {
                core::OptionStrategyEngine* se = main->option_strategy_engine();
                if (se != nullptr && se->get_strategy() != nullptr) {
                    strategy_name = se->get_strategy()->strategy_name();
                }
            }
            ex->store_order(strategy_name, *ord);
        }
        if (main->position_engine() != nullptr) {
            main->position_engine()->process_order(strategy_name, *ord);
        }
        if (main->option_strategy_engine() != nullptr) {
            main->option_strategy_engine()->process_order(*ord);
        }
    }
}

void EventEngine::dispatch_trade(const utilities::Event& event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    if (const auto* slot = std::get_if<utilities::TradeData*>(&event.data)) {
        utilities::TradeData* tr = *slot;
        if (tr == nullptr) {
            return;
        }
        core::ExecutionEngine* ex = main->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            ex->store_trade(*tr);
            strategy_name = ex->get_strategy_name_for_order(tr->orderid);
            if (strategy_name.empty()) {
                core::OptionStrategyEngine* se = main->option_strategy_engine();
                if (se != nullptr && se->get_strategy() != nullptr) {
                    strategy_name = se->get_strategy()->strategy_name();
                }
            }
        }
        if (main->position_engine() != nullptr) {
            main->position_engine()->process_trade(strategy_name, *tr);
        }
        if (main->option_strategy_engine() != nullptr) {
            main->option_strategy_engine()->process_trade(*tr);
        }
    }
}

} // namespace backtest
