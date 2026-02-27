/**
 * Live event engine: queue + worker thread; dispatch control in Event (same as backtest).
 */

#include "engine_event.hpp"
#include "../../core/engine_execution.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../utilities/event.hpp"
#include "engine_main.hpp"
#include <chrono>
#include <thread>
#include <variant>

namespace engines {

EventEngine::EventEngine(int interval) : interval_(interval) {}

EventEngine::~EventEngine() { stop(); }

void EventEngine::start() {
    if (active_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&EventEngine::run, this);
    timer_thread_ = std::thread(&EventEngine::run_timer, this);
}

void EventEngine::stop() {
    if (!active_.exchange(false)) {
        return;
    }
    queue_cv_.notify_all();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

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

void EventEngine::put_event(const utilities::Event& event) { put(event); }

void EventEngine::put(const utilities::Event& event) {
    {
        std::scoped_lock lock(queue_mutex_);
        queue_.push(event);
    }
    queue_cv_.notify_one();
}

void EventEngine::put(utilities::Event&& event) {
    {
        std::scoped_lock lock(queue_mutex_);
        queue_.push(std::move(event));
    }
    queue_cv_.notify_one();
}

void EventEngine::process(const utilities::Event& event) {
    if (main_engine_ == nullptr) {
        return;
    }
    switch (event.type) {
    case utilities::EventType::Snapshot:
        dispatch_snapshot(event);
        break;
    case utilities::EventType::Timer:
        dispatch_timer();
        break;
    case utilities::EventType::Order:
        dispatch_order(event);
        break;
    case utilities::EventType::Trade:
        dispatch_trade(event);
        break;
    case utilities::EventType::Contract:
        dispatch_contract(event);
        break;
    default:
        break;
    }
}

void EventEngine::dispatch_timer() {
    if (main_engine_->ib_gateway() != nullptr) {
        main_engine_->ib_gateway()->process_timer_event(
            utilities::Event(utilities::EventType::Timer));
    }
    engines::PositionEngine* pos = main_engine_->position_engine();
    if (pos != nullptr) {
        std::vector<utilities::LogData> pos_logs;
        pos->process_timer_event(
            [this](const std::string& name) -> utilities::PortfolioData* {
                return main_engine_->get_portfolio(name);
            },
            &pos_logs);
        for (const auto& l : pos_logs) {
            main_engine_->put_log_intent(l);
        }
    }
    engines::HedgeEngine* hedge = main_engine_->hedge_engine();
    core::OptionStrategyEngine* se = main_engine_->option_strategy_engine();
    if ((hedge != nullptr) && (se != nullptr)) {
        for (const std::string& strategy_name : se->get_strategy_names()) {
            std::string portfolio_name = strategy_name;
            size_t p = strategy_name.find('_');
            if (p != std::string::npos && p + 1 < strategy_name.size()) {
                portfolio_name = strategy_name.substr(p + 1);
            }
            engines::HedgeParams params;
            params.portfolio = main_engine_->get_portfolio(portfolio_name);
            params.holding = main_engine_->get_holding(strategy_name);
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
            std::vector<utilities::OrderRequest> orders;
            std::vector<utilities::CancelRequest> cancels;
            std::vector<utilities::LogData> logs;
            hedge->process_hedging(strategy_name, params, &orders, &cancels, &logs);
            for (const auto& o : orders) {
                main_engine_->send_order(o);
            }
            for (const auto& c : cancels) {
                main_engine_->cancel_order(c);
            }
            for (const auto& l : logs) {
                main_engine_->put_log_intent(l);
            }
        }
    }
    if (se != nullptr) {
        se->on_timer();
    }
}

void EventEngine::dispatch_order(const utilities::Event& event) {
    if (const auto* pd = std::get_if<utilities::OrderData>(&event.data)) {
        utilities::OrderData order = *pd;
        core::ExecutionEngine* ex = main_engine_->execution_engine();
        if (ex != nullptr) {
            std::string strategy_name = ex->get_strategy_name_for_order(order.orderid);
            ex->store_order(strategy_name, order);
            if (!strategy_name.empty()) {
                main_engine_->save_order_data(strategy_name, order);
            }
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
    if (const auto* pd = std::get_if<utilities::TradeData>(&event.data)) {
        utilities::TradeData trade = *pd;
        core::ExecutionEngine* ex = main_engine_->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            ex->store_trade(trade);
            strategy_name = ex->get_strategy_name_for_order(trade.orderid);
            if (!strategy_name.empty()) {
                main_engine_->save_trade_data(strategy_name, trade);
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

void EventEngine::dispatch_snapshot(const utilities::Event& event) {
    if (const auto* snap = std::get_if<utilities::PortfolioSnapshot>(&event.data)) {
        utilities::PortfolioData* portfolio = main_engine_->get_portfolio(snap->portfolio_name);
        if (portfolio != nullptr) {
            portfolio->apply_frame(*snap);
        }
    }
}

void EventEngine::dispatch_contract(const utilities::Event& event) {
    if (main_engine_->market_data_engine() != nullptr) {
        main_engine_->market_data_engine()->process_contract(event);
    }
}

void EventEngine::run_timer() {
    while (active_) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_));
        if (!active_) {
            break;
        }
        put(utilities::Event(utilities::EventType::Timer));
    }
}

void EventEngine::run() {
    while (active_) {
        utilities::Event event;
        bool got = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::seconds(1), [this, &got, &event]() -> bool {
                if (!active_) {
                    return true;
                }
                if (queue_.empty()) {
                    return false;
                }
                event = std::move(queue_.front());
                queue_.pop();
                got = true;
                return true;
            });
        }
        if (got && active_) {
            process(event);
        }
    }
}

} // namespace engines
