#include "engine_main.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../strategy/template.hpp"
#include "../../utilities/intent.hpp"
#include "../../utilities/utility.hpp"
#include "engine_data_historical.hpp"
#include <stdexcept>
#include <utility>

namespace backtest {

MainEngine::MainEngine() {
    event_engine_ = std::make_unique<EventEngine>(this);
    event_engine_->start();

    init_core(event_engine_.get(),
              CoreInitParams{
                  .send_impl = [this](const utilities::OrderRequest& req) -> std::string {
                      return send_order(req);
                  },
                  .cancel_impl =
                      [this](core::ExecutionEngine* exe, const utilities::CancelRequest& req) {
                          utilities::OrderData* o = exe ? exe->get_order(req.orderid) : nullptr;
                          if (o != nullptr) {
                              o->status = utilities::Status::CANCELLED;
                              utilities::OrderData* p = acquire_order();
                              if (p != nullptr) {
                                  *p = *o;
                                  put_event(utilities::Event(utilities::EventType::Order, p));
                              }
                          }
                      },
                  .put_strategy_event = nullptr,
                  .log_level = engines::DISABLED,
              });

    put_log_intent("Main engine initialization successful", INFO);
}

MainEngine::~MainEngine() = default;

// ---- Infra hooks ----

std::string MainEngine::send_order_to_gateway(const utilities::OrderRequest& req) {
    if (!order_executor_) {
        throw std::runtime_error(
            "No order executor set. Use BacktestEngine for backtest execution.");
    }
    return order_executor_(req);
}

void MainEngine::close_infra() {
    if (event_engine_) {
        event_engine_->close();
    }
}

// ---- Backtest-specific methods ----

auto MainEngine::load_backtest_data(const std::string& parquet_path,
                                    const std::string& underlying_symbol) -> BacktestDataEngine* {
    if (!data_engine_) {
        data_engine_ = std::make_unique<BacktestDataEngine>(this);
    }
    data_engine_->load_parquet(parquet_path, "ts_recv", underlying_symbol);
    put_log_intent("Backtest data loaded from: " + parquet_path, INFO);
    return data_engine_.get();
}

void MainEngine::put_event(const utilities::Event& e) { event_engine_->put_event(e); }

void MainEngine::put_event(utilities::Event&& e) {
    event_engine_->put_event(std::forward<utilities::Event>(e));
}

auto MainEngine::send_order(const utilities::OrderRequest& req) -> std::string {
    return send_order_to_gateway(req);
}

auto MainEngine::send_order(const std::string& strategy_name, const utilities::OrderRequest& req)
    -> std::string {
    return execution_engine_ ? execution_engine_->send_order(strategy_name, req) : std::string{};
}

void MainEngine::add_order(std::string orderid, utilities::OrderData order) {
    order.orderid = std::move(orderid);
    if (execution_engine_) {
        execution_engine_->add_order(order);
    }
}

void MainEngine::cancel_order(const utilities::CancelRequest& req) {
    if (cancel_executor_) {
        cancel_executor_(req);
    }
    if (execution_engine_) {
        execution_engine_->cancel_order(req);
    }
}

auto MainEngine::get_order(const std::string& orderid) const -> utilities::OrderData* {
    return execution_engine_ ? execution_engine_->get_order(orderid) : nullptr;
}

auto MainEngine::get_trade(const std::string& tradeid) const -> utilities::TradeData* {
    return execution_engine_ ? execution_engine_->get_trade(tradeid) : nullptr;
}

auto MainEngine::get_all_orders() const -> std::vector<utilities::OrderData> {
    return option_strategy_engine() ? option_strategy_engine()->get_all_orders()
                                    : std::vector<utilities::OrderData>{};
}

auto MainEngine::get_all_trades() const -> std::vector<utilities::TradeData> {
    return option_strategy_engine() ? option_strategy_engine()->get_all_trades()
                                    : std::vector<utilities::TradeData>{};
}

auto MainEngine::get_all_active_orders() const -> std::vector<utilities::OrderData> {
    return option_strategy_engine() ? option_strategy_engine()->get_all_active_orders()
                                    : std::vector<utilities::OrderData>{};
}

void MainEngine::put_log_intent(const std::string& msg, int level) const {
    utilities::LogData intent;
    intent.msg = msg;
    intent.level = level;
    intent.gateway_name = "Main";
    // Call base class virtual (non-const, but safe since log_engine_ is mutable-equivalent).
    const_cast<MainEngine*>(this)->runtime_common::MainEngineBase::put_log_intent(intent);
}

auto MainEngine::acquire_snapshot() -> utilities::PortfolioSnapshot* {
    return event_engine_ ? event_engine_->acquire_snapshot() : nullptr;
}

auto MainEngine::acquire_order() -> utilities::OrderData* {
    return event_engine_ ? event_engine_->acquire_order() : nullptr;
}

auto MainEngine::acquire_trade() -> utilities::TradeData* {
    return event_engine_ ? event_engine_->acquire_trade() : nullptr;
}

} // namespace backtest
