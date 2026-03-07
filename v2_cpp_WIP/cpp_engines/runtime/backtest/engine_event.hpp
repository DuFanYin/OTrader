#pragma once

/** EventEngine: dispatch by event type and fixed order; no engines held; access via Main. */

#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/intent.hpp"
#include "../../utilities/portfolio.hpp"
#include <optional>
#include <string>
#include <vector>

namespace backtest {

class MainEngine;

class EventEngine : public utilities::BaseEngine {
  public:
    explicit EventEngine(utilities::MainEngine* main) : BaseEngine(main, "Event") {}
    ~EventEngine() override = default;

    void start() {}
    void stop() {}

    void close() override { stop(); }

    /** Intent entry; SendOrder→orderid, others→nullopt. */
    std::optional<std::string> put_intent(const utilities::Intent& intent);
    void put_event(const utilities::Event& event);

  private:
    void dispatch_snapshot(const utilities::Event& event);
    void dispatch_timer(std::vector<utilities::OrderRequest>* out_orders,
                        std::vector<utilities::CancelRequest>* out_cancels,
                        std::vector<utilities::LogData>* out_logs);
    void dispatch_order(const utilities::Event& event);
    void dispatch_trade(const utilities::Event& event);
};

} // namespace backtest
