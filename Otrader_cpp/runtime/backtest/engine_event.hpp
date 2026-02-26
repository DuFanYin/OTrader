#pragma once

/**
 * EventEngine = dispatch 控制逻辑：按事件类型与固定顺序决定调用谁。
 * 不持有任何引擎；通过 MainEngine 的 accessor 访问各引擎并调用，执行（send_order 等）也通过 Main。
 */

#include "../../utilities/event.hpp"
#include "../../utilities/portfolio.hpp"
#include <vector>

namespace backtest {

class MainEngine;

class EventEngine : public utilities::IEventEngine {
  public:
    EventEngine() = default;
    ~EventEngine() override = default;

    void start() override {}
    void stop() override {}

    std::string put_intent_send_order(const utilities::OrderRequest& req) override;
    void put_intent_cancel_order(const utilities::CancelRequest& req) override;
    void put_intent_log(const utilities::LogData& log) override;
    void put_event(const utilities::Event& event) override;

    void set_main_engine(MainEngine* main_engine) { main_engine_ = main_engine; }

  private:
    void dispatch_snapshot(const utilities::Event& event);
    void dispatch_timer(std::vector<utilities::OrderRequest>* out_orders,
                        std::vector<utilities::CancelRequest>* out_cancels,
                        std::vector<utilities::LogData>* out_logs);
    void dispatch_order(const utilities::Event& event);
    void dispatch_trade(const utilities::Event& event);

    MainEngine* main_engine_ = nullptr;
};

} // namespace backtest
