#pragma once

/** EventEngine: dispatch by event type and fixed order; no engines held; access via Main. */

#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/intent.hpp"
#include "../../utilities/object_pool.hpp"
#include "../../utilities/portfolio.hpp"
#include <functional>
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
    void put_event(utilities::Event&& event);

    utilities::PortfolioSnapshot* acquire_snapshot() { return snapshot_pool_.acquire(); }
    void release_snapshot(utilities::PortfolioSnapshot* p) { snapshot_pool_.release(p); }
    utilities::OrderData* acquire_order() { return order_pool_.acquire(); }
    void release_order(utilities::OrderData* p) { order_pool_.release(p); }
    utilities::TradeData* acquire_trade() { return trade_pool_.acquire(); }
    void release_trade(utilities::TradeData* p) { trade_pool_.release(p); }

  private:
    void release_event_payload(utilities::Event* e);
    void run_dispatch(utilities::Event* p);
    void dispatch_snapshot(const utilities::Event& event);
    void dispatch_timer(std::vector<utilities::OrderRequest>* out_orders,
                        std::vector<utilities::CancelRequest>* out_cancels,
                        std::vector<utilities::LogData*>* out_logs,
                        const std::function<utilities::LogData*()>& acquire_log = nullptr);
    void dispatch_order(const utilities::Event& event);
    void dispatch_trade(const utilities::Event& event);

    utilities::ObjectPool<utilities::Event> event_pool_;
    utilities::ObjectPool<utilities::PortfolioSnapshot> snapshot_pool_;
    utilities::ObjectPool<utilities::OrderData> order_pool_;
    utilities::ObjectPool<utilities::TradeData> trade_pool_;
};

} // namespace backtest
