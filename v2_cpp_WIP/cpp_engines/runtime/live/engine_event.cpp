/**
 * Live event engine: queue + worker thread; dispatch control in Event (same as backtest).
 */

#include "engine_event.hpp"
#include "../../core/engine_execution.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/intent.hpp"
#include "engine_main.hpp"
#include <chrono>
#include <format>
#include <thread>
#include <utility>
#include <variant>

namespace engines {

EventEngine::EventEngine(utilities::MainEngine* main, int interval)
    : BaseEngine(main, "Event"), interval_(interval) {}

EventEngine::~EventEngine() { EventEngine::stop(); }

void EventEngine::start() {
    if (active_.exchange(true)) {
        return;
    }
    thread_ = std::jthread([this](std::stop_token st) { run(std::move(st)); });
    timer_thread_ = std::jthread([this](std::stop_token st) { run_timer(std::move(st)); });
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

void EventEngine::close() { stop(); }

auto EventEngine::put_intent(const utilities::Intent& intent) -> std::optional<std::string> {
    auto* main = static_cast<MainEngine*>(main_engine);
    switch (static_cast<utilities::IntentType>(intent.index())) {
        using enum utilities::IntentType;
    case SendOrder: {
        const auto& arg = std::get<utilities::IntentSendOrder>(intent);
        if (main == nullptr || main->execution_engine() == nullptr) {
            if (main != nullptr) {
                utilities::LogData log;
                log.msg = std::format(
                    "[EventEngine] send_order failed: execution_engine is null for strategy {}",
                    arg.strategy_name);
                log.level = ERROR;
                log.gateway_name = "Event";
                main->put_log_intent(log);
            }
            return std::nullopt;
        }
        auto* ex = main->execution_engine();
        std::string orderid = ex->send_order(arg.strategy_name, arg.req);
        if (orderid.empty()) {
            std::string combo_str =
                arg.req.combo_type.has_value() ? utilities::to_string(*arg.req.combo_type) : "";
            utilities::LogData log;
            log.msg = std::format("[EventEngine] send_order returned empty orderid strategy={} "
                                  "symbol={} is_combo={} type={} dir={} vol={}{}",
                                  arg.strategy_name, arg.req.symbol, arg.req.is_combo ? 1 : 0,
                                  utilities::to_string(arg.req.type),
                                  utilities::to_string(arg.req.direction), arg.req.volume,
                                  combo_str.empty() ? "" : " combo_type=" + combo_str);
            log.level = ERROR;
            log.gateway_name = "Event";
            main->put_log_intent(log);
        }
        return orderid;
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
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    switch (event.type) {
        using enum utilities::EventType;
    case Snapshot:
        dispatch_snapshot(event);
        break;
    case Timer:
        dispatch_timer();
        break;
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

void EventEngine::dispatch_timer() {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    if (main->ib_gateway() != nullptr) {
        main->ib_gateway()->process_timer_event(utilities::Event(utilities::EventType::Timer));
    }
    engines::PositionEngine* pos = main->position_engine();
    if (pos != nullptr) {
        std::vector<utilities::LogData> pos_logs;
        pos->process_timer_event(
            [main](const std::string& name) -> utilities::PortfolioData* {
                return main->get_portfolio(name);
            },
            &pos_logs);
        for (const auto& l : pos_logs) {
            main->put_log_intent(l);
        }
    }
    engines::HedgeEngine* hedge = main->hedge_engine();
    core::OptionStrategyEngine* se = main->option_strategy_engine();
    if ((hedge != nullptr) && (se != nullptr)) {
        for (const std::string& strategy_name : se->get_strategy_names()) {
            std::string portfolio_name = strategy_name;
            size_t p = strategy_name.find('_');
            if (p != std::string::npos && p + 1 < strategy_name.size()) {
                portfolio_name = strategy_name.substr(p + 1);
            }
            engines::HedgeParams params;
            params.portfolio = main->get_portfolio(portfolio_name);
            params.holding = main->get_holding(strategy_name);
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
            std::vector<utilities::OrderRequest> orders;
            std::vector<utilities::CancelRequest> cancels;
            std::vector<utilities::LogData> logs;
            hedge->process_hedging(strategy_name, params, &orders, &cancels, &logs);
            for (const auto& o : orders) {
                put_intent(utilities::IntentSendOrder{strategy_name, o});
            }
            for (const auto& c : cancels) {
                put_intent(utilities::IntentCancelOrder{c});
            }
            for (const auto& l : logs) {
                put_intent(utilities::IntentLog{l});
            }
        }
    }
    if (se != nullptr) {
        se->on_timer();
    }
}

void EventEngine::dispatch_order(const utilities::Event& event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    if (const auto* pd = std::get_if<utilities::OrderData>(&event.data)) {
        utilities::OrderData order = *pd;
        core::ExecutionEngine* ex = main->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            strategy_name = ex->get_strategy_name_for_order(order.orderid);
            ex->store_order(strategy_name, order);
            if (!strategy_name.empty()) {
                main->save_order_data(strategy_name, order);
            }
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
    if (const auto* pd = std::get_if<utilities::TradeData>(&event.data)) {
        utilities::TradeData trade = *pd;
        core::ExecutionEngine* ex = main->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            ex->store_trade(trade);
            strategy_name = ex->get_strategy_name_for_order(trade.orderid);
            if (!strategy_name.empty()) {
                main->save_trade_data(strategy_name, trade);
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

void EventEngine::run_timer(const std::stop_token& st) {
    while (!st.stop_requested() && active_) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_));
        if (!st.stop_requested() && active_) {
            put(utilities::Event(utilities::EventType::Timer));
        }
    }
}

void EventEngine::run(const std::stop_token& st) {
    while (!st.stop_requested() && active_) {
        utilities::Event event;
        bool got = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, st, [this, &got, &event]() -> bool {
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
