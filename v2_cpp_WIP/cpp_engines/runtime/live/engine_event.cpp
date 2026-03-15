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
    thread_ = std::jthread([this](const std::stop_token& st) { run(st); });
    timer_thread_ = std::jthread([this](const std::stop_token& st) { run_timer(st); });
    strategy_thread_ = std::jthread([this](const std::stop_token& st) { run_strategy(st); });
}

void EventEngine::stop() {
    if (!active_.exchange(false)) {
        return;
    }
    queue_cv_.notify_all();
    strategy_cv_.notify_all();
    if (strategy_thread_.joinable()) {
        strategy_thread_.join();
    }
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    // Drain queues: release any pooled pointer in payload, then return Event* to pool
    auto release_event = [this](utilities::Event* e) {
        release_event_payload(e);
        event_pool_.release(e);
    };
    utilities::Event* e = nullptr;
    while (queue_ring_.try_pop(e)) {
        release_event(e);
    }
    while (strategy_ring_.try_pop(e)) {
        release_event(e);
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

void EventEngine::put_event(utilities::Event&& event) {
    put(std::forward<utilities::Event>(event));
}

void EventEngine::put(const utilities::Event& event) {
    utilities::Event* p = event_pool_.acquire();
    if (p == nullptr) {
        return;
    }
    *p = event;
    if (queue_ring_.try_push(p)) {
        queue_cv_.notify_one();
        return;
    }
    for (int spin = 0; spin < 300 && !queue_ring_.try_push(p); ++spin) {
        std::this_thread::yield();
    }
    if (!queue_ring_.try_push(p)) {
        event_pool_.release(p);
    } else {
        queue_cv_.notify_one();
    }
}

void EventEngine::put(utilities::Event&& event) {
    utilities::Event* p = event_pool_.acquire();
    if (p == nullptr) {
        return;
    }
    *p = event;
    if (queue_ring_.try_push(p)) {
        queue_cv_.notify_one();
        return;
    }
    for (int spin = 0; spin < 300 && !queue_ring_.try_push(p); ++spin) {
        std::this_thread::yield();
    }
    if (!queue_ring_.try_push(p)) {
        event_pool_.release(p);
    } else {
        queue_cv_.notify_one();
    }
}

void EventEngine::process(utilities::Event* event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr || event == nullptr) {
        return;
    }
    switch (event->type) {
        using enum utilities::EventType;
    case Snapshot:
        dispatch_snapshot(*event);
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
    engines::PositionEngine* pos = main->position_engine();
    if (pos != nullptr) {
        pos->process_timer_event([main](const std::string& name) -> utilities::PortfolioData* {
            return main->get_portfolio(name);
        });
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
            std::vector<utilities::LogData*> logs;
            hedge->process_hedging(strategy_name, params, &orders, &cancels, &logs,
                                   [main]() { return main->acquire_log(); });
            for (const auto& o : orders) {
                put_intent(utilities::IntentSendOrder{strategy_name, o});
            }
            for (const auto& c : cancels) {
                put_intent(utilities::IntentCancelOrder{c});
            }
            for (utilities::LogData* p : logs) {
                put_intent(utilities::IntentLog{*p});
                main->release_log(p);
            }
        }
    }
    utilities::Event* p = event_pool_.acquire();
    if (p != nullptr) {
        p->type = utilities::EventType::Timer;
        p->data = std::monostate{};
        if (!strategy_ring_.try_push(p)) {
            event_pool_.release(p);
        } else {
            strategy_cv_.notify_one();
        }
    }
}

void EventEngine::dispatch_order(utilities::Event* event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr || event == nullptr) {
        return;
    }
    if (const auto* slot = std::get_if<utilities::OrderData*>(&event->data)) {
        utilities::OrderData* ord = *slot;
        if (ord == nullptr) {
            return;
        }
        core::ExecutionEngine* ex = main->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            strategy_name = ex->get_strategy_name_for_order(ord->orderid);
            ex->store_order(strategy_name, *ord);
            if (!strategy_name.empty()) {
                main->save_order_data(strategy_name, *ord);
            }
        }
        if (main->position_engine() != nullptr) {
            main->position_engine()->process_order(strategy_name, *ord);
        }
        utilities::Event* p = event_pool_.acquire();
        if (p != nullptr) {
            p->type = utilities::EventType::Order;
            p->data = ord;
            if (strategy_ring_.try_push(p)) {
                strategy_cv_.notify_one();
            } else {
                for (int spin = 0; spin < 200 && !strategy_ring_.try_push(p); ++spin) {
                    std::this_thread::yield();
                }
                if (!strategy_ring_.try_push(p)) {
                    event_pool_.release(p);
                } else {
                    strategy_cv_.notify_one();
                }
            }
        }
        event->data = std::monostate{};
    }
}

void EventEngine::dispatch_trade(utilities::Event* event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr || event == nullptr) {
        return;
    }
    if (const auto* slot = std::get_if<utilities::TradeData*>(&event->data)) {
        utilities::TradeData* tr = *slot;
        if (tr == nullptr) {
            return;
        }
        core::ExecutionEngine* ex = main->execution_engine();
        std::string strategy_name;
        if (ex != nullptr) {
            ex->store_trade(*tr);
            strategy_name = ex->get_strategy_name_for_order(tr->orderid);
            if (!strategy_name.empty()) {
                main->save_trade_data(strategy_name, *tr);
            }
        }
        if (main->position_engine() != nullptr) {
            main->position_engine()->process_trade(strategy_name, *tr);
        }
        utilities::Event* p = event_pool_.acquire();
        if (p != nullptr) {
            p->type = utilities::EventType::Trade;
            p->data = tr;
            if (strategy_ring_.try_push(p)) {
                strategy_cv_.notify_one();
            } else {
                for (int spin = 0; spin < 200 && !strategy_ring_.try_push(p); ++spin) {
                    std::this_thread::yield();
                }
                if (!strategy_ring_.try_push(p)) {
                    event_pool_.release(p);
                } else {
                    strategy_cv_.notify_one();
                }
            }
        }
        event->data = std::monostate{};
    }
}

void EventEngine::process_strategy(const utilities::Event& event) {
    auto* main = static_cast<MainEngine*>(main_engine);
    if (main == nullptr) {
        return;
    }
    core::OptionStrategyEngine* se = main->option_strategy_engine();
    if (se == nullptr) {
        return;
    }
    switch (event.type) {
        using enum utilities::EventType;
    case Timer:
        se->on_timer();
        break;
    case Order:
        if (const auto* slot = std::get_if<utilities::OrderData*>(&event.data)) {
            if (*slot != nullptr) {
                se->process_order(**slot);
            }
        }
        break;
    case Trade:
        if (const auto* slot = std::get_if<utilities::TradeData*>(&event.data)) {
            if (*slot != nullptr) {
                se->process_trade(**slot);
            }
        }
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
            // Do not release here; release_event_payload() releases after process().
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

void EventEngine::run_strategy(const std::stop_token& st) {
    while (!st.stop_requested() && active_) {
        utilities::Event* event_ptr = nullptr;
        {
            std::unique_lock lock(strategy_mutex_);
            strategy_cv_.wait(lock, st,
                              [this]() -> bool { return !active_ || !strategy_ring_.empty(); });
        }
        if (!active_) {
            break;
        }
        if (strategy_ring_.try_pop(event_ptr) && event_ptr != nullptr) {
            process_strategy(*event_ptr);
            release_event_payload(event_ptr);
            event_pool_.release(event_ptr);
        }
    }
}

void EventEngine::run(const std::stop_token& st) {
    while (!st.stop_requested() && active_) {
        utilities::Event* event_ptr = nullptr;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, st, [this]() -> bool { return !active_ || !queue_ring_.empty(); });
        }
        if (!active_) {
            break;
        }
        if (queue_ring_.try_pop(event_ptr) && event_ptr != nullptr) {
            process(event_ptr);
            release_event_payload(event_ptr);
            event_pool_.release(event_ptr);
        }
    }
}

} // namespace engines
