#pragma once

#include "../../utilities/constant.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/portfolio.hpp"
#include "../main_engine_base.hpp"
#include "engine_event.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace backtest {

class BacktestDataEngine;

class MainEngine : public runtime_common::MainEngineBase {
  public:
    static constexpr int INFO = 20;

    MainEngine();
    ~MainEngine() override;

    BacktestDataEngine* load_backtest_data(const std::string& parquet_path,
                                           const std::string& underlying_symbol = "");
    BacktestDataEngine* get_data_engine() const { return data_engine_.get(); }

    std::string send_order(const utilities::OrderRequest& req);
    std::string send_order(const std::string& strategy_name,
                           const utilities::OrderRequest& req) override;
    void add_order(std::string orderid, utilities::OrderData order);
    void cancel_order(const utilities::CancelRequest& req) override;

    utilities::OrderData* get_order(const std::string& orderid) const;
    utilities::TradeData* get_trade(const std::string& tradeid) const;
    std::vector<utilities::OrderData> get_all_orders() const;
    std::vector<utilities::TradeData> get_all_trades() const;
    std::vector<utilities::OrderData> get_all_active_orders() const;

    void put_event(const utilities::Event& e) override;
    void put_event(utilities::Event&& e) override;

    using MainEngineBase::put_log_intent;
    void put_log_intent(const std::string& msg, int level = INFO) const;

    utilities::PortfolioSnapshot* acquire_snapshot() override;
    utilities::OrderData* acquire_order() override;
    utilities::TradeData* acquire_trade() override;

    EventEngine* event_engine() { return event_engine_.get(); }
    const EventEngine* event_engine() const { return event_engine_.get(); }

    using OrderExecutor = std::function<std::string(const utilities::OrderRequest&)>;
    void set_order_executor(OrderExecutor fn) { order_executor_ = std::move(fn); }

    using CancelExecutor = std::function<void(const utilities::CancelRequest&)>;
    void set_cancel_executor(CancelExecutor fn) { cancel_executor_ = std::move(fn); }

    // ---- Infra hooks ----
    std::string send_order_to_gateway(const utilities::OrderRequest& req) override;
    void close_infra() override;

  private:
    std::unique_ptr<EventEngine> event_engine_;
    int order_counter_ = 0;
    OrderExecutor order_executor_;
    CancelExecutor cancel_executor_;
    std::unique_ptr<BacktestDataEngine> data_engine_;
};

} // namespace backtest
