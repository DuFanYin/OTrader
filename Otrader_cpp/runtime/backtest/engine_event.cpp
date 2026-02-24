#include "engine_event.hpp"
#include "engine_main.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../strategy/template.hpp"
#include <chrono>
#include <variant>

namespace backtest {

std::string EventEngine::put_intent_send_order(const utilities::OrderRequest& req) {
    if (main_engine_) return main_engine_->send_order(req);
    return {};
}

void EventEngine::put_intent_cancel_order(const utilities::CancelRequest& req) {
    if (main_engine_) main_engine_->cancel_order(req);
}

void EventEngine::put_intent_log(const utilities::LogData& log) {
    if (main_engine_) main_engine_->put_log_intent(log);
}

void EventEngine::put_event(const utilities::Event& event) {
    if (!main_engine_) return;
    std::vector<utilities::OrderRequest> orders;
    std::vector<utilities::CancelRequest> cancels;
    std::vector<utilities::LogData> logs;
    switch (event.type) {
        case utilities::EventType::Snapshot:
            dispatch_snapshot(event);
            break;
        case utilities::EventType::Timer: {
            dispatch_timer(&orders, &cancels, &logs);
            for (const auto& o : orders)
                main_engine_->send_order(o);
            for (const auto& c : cancels)
                main_engine_->cancel_order(c);
            for (const auto& l : logs)
                main_engine_->put_log_intent(l);
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
        if (portfolio)
            portfolio->apply_frame(*snap);
    }
}

void EventEngine::dispatch_timer(std::vector<utilities::OrderRequest>* out_orders,
                                  std::vector<utilities::CancelRequest>* out_cancels,
                                  std::vector<utilities::LogData>* out_logs) {
    core::OptionStrategyEngine* se = main_engine_->option_strategy_engine();
    if (!se || !se->get_strategy()) return;
    se->get_strategy()->on_timer();
    engines::PositionEngine* pos = main_engine_->position_engine();
    if (pos && se->get_strategy()) {
        auto* portfolio = main_engine_->get_portfolio(se->get_strategy()->portfolio_name());
        if (portfolio)
            pos->update_metrics(se->get_strategy()->strategy_name(), portfolio);
    }
    engines::HedgeEngine* hedge = main_engine_->hedge_engine();
    if (hedge && se->get_strategy() && (out_orders || out_cancels || out_logs)) {
        engines::HedgeParams params;
        params.portfolio = main_engine_->get_portfolio(se->get_strategy()->portfolio_name());
        params.holding = main_engine_->get_holding(se->get_strategy()->strategy_name());
        params.get_contract = [this](const std::string& sym) { return main_engine_->get_contract(sym); };
        params.get_strategy_active_orders = [se]() -> const std::unordered_map<std::string, std::set<std::string>>& {
            return se->get_strategy_active_orders();
        };
        params.get_order = [se](const std::string& oid) { return se->get_order(oid); };
        hedge->process_hedging(se->get_strategy()->strategy_name(), params, out_orders, out_cancels, out_logs);
    }
}

void EventEngine::dispatch_order(const utilities::Event& event) {
    if (const auto* p = std::get_if<utilities::OrderData>(&event.data)) {
        utilities::OrderData order = *p;
        if (main_engine_->position_engine()) main_engine_->position_engine()->process_order(order);
        if (main_engine_->option_strategy_engine()) main_engine_->option_strategy_engine()->process_order(order);
    }
}

void EventEngine::dispatch_trade(const utilities::Event& event) {
    core::OptionStrategyEngine* se = main_engine_->option_strategy_engine();
    if (!se || !se->get_strategy()) return;
    if (const auto* p = std::get_if<utilities::TradeData>(&event.data)) {
        const std::string& strategy_name = se->get_strategy()->strategy_name();
        if (main_engine_->position_engine())
            main_engine_->position_engine()->process_trade(strategy_name, *p);
        se->process_trade(*p);
    }
}

}  // namespace backtest
