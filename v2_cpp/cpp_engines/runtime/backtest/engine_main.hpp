#pragma once

/** Backtest MainEngine: holds engines, EventEngine dispatch, accessors. */

#include "../../core/engine_execution.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_log.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../core/engine_position.hpp"
#include "../../core/portfolio_structure.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/constant.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/portfolio.hpp"
#include "engine_event.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace backtest {

class BacktestDataEngine;

class MainEngine : public utilities::MainEngine {
  public:
    static constexpr int INFO = 20;

    MainEngine();
    ~MainEngine();

    engines::PortfolioStructure* portfolio_structure() { return portfolio_structure_.get(); }
    const engines::PortfolioStructure* portfolio_structure() const {
        return portfolio_structure_.get();
    }
    utilities::PortfolioData* get_portfolio(const std::string& portfolio_name) const;
    const utilities::ContractData* get_contract(const std::string& symbol) const;

    BacktestDataEngine* load_backtest_data(const std::string& parquet_path,
                                           const std::string& underlying_symbol = "");
    BacktestDataEngine* get_data_engine() const { return data_engine_.get(); }

    std::string send_order(const utilities::OrderRequest& req);
    void add_order(std::string orderid, utilities::OrderData order);
    void cancel_order(const utilities::CancelRequest& req);

    utilities::OrderData* get_order(const std::string& orderid) const;
    utilities::TradeData* get_trade(const std::string& tradeid) const;
    std::vector<utilities::OrderData> get_all_orders() const;
    std::vector<utilities::TradeData> get_all_trades() const;
    std::vector<utilities::OrderData> get_all_active_orders() const;

    void put_event(const utilities::Event& e) override;
    void put_event(utilities::Event&& e) override;
    void put_event(utilities::Event& e) override;

    /** Forward write_log to put_log_intent. */
    void write_log(const std::string& msg, int level = INFO,
                   const std::string& gateway = "") override;

    /** Emit LogIntent; Runtime sinks it (no event bus). */
    void put_log_intent(const std::string& msg, int level = INFO) const;
    /** Emit LogIntent from payload. */
    void put_log_intent(const utilities::LogData& intent) const;
    /** Pooled log: acquire, fill, put_log_intent(*p), release_log(p). */
    utilities::LogData* acquire_log();
    void release_log(utilities::LogData* p);
    /** Pooled snapshot for Snapshot events; EventEngine releases on dispatch. */
    utilities::PortfolioSnapshot* acquire_snapshot() override;
    utilities::OrderData* acquire_order() override;
    utilities::TradeData* acquire_trade() override;
    void close();

    /** append_* → send_order/cancel_order/put_log_intent. */
    std::string append_order(const utilities::OrderRequest& req);
    void append_cancel(const utilities::CancelRequest& req);
    void append_log(const utilities::LogData& log) const;

    /** Log level threshold; DISABLED = off. */
    void set_log_level(int level);
    int log_level() const;

    EventEngine* event_engine() { return event_engine_.get(); }
    const EventEngine* event_engine() const { return event_engine_.get(); }

    core::ExecutionEngine* execution_engine() { return execution_engine_.get(); }
    core::OptionStrategyEngine* option_strategy_engine() { return option_strategy_engine_.get(); }
    const core::OptionStrategyEngine* option_strategy_engine() const {
        return option_strategy_engine_.get();
    }

    engines::PositionEngine* position_engine() { return position_engine_.get(); }
    const engines::PositionEngine* position_engine() const { return position_engine_.get(); }
    engines::HedgeEngine* hedge_engine();
    utilities::StrategyHolding* get_holding(const std::string& strategy_name);
    const utilities::StrategyHolding* get_holding(const std::string& strategy_name) const;
    void get_or_create_holding(const std::string& strategy_name);

    using OrderExecutor = std::function<std::string(const utilities::OrderRequest&)>;
    void set_order_executor(OrderExecutor fn) { order_executor_ = std::move(fn); }

  private:
    std::unique_ptr<EventEngine> event_engine_;
    std::unique_ptr<engines::PortfolioStructure> portfolio_structure_;
    int order_counter_ = 0;
    OrderExecutor order_executor_;
    std::unique_ptr<core::ExecutionEngine> execution_engine_;
    std::unique_ptr<core::OptionStrategyEngine> option_strategy_engine_;
    std::unique_ptr<BacktestDataEngine> data_engine_;
    std::unique_ptr<engines::PositionEngine> position_engine_;
    std::unique_ptr<engines::HedgeEngine> hedge_engine_;
    std::unique_ptr<engines::LogEngine> log_engine_;
    std::unordered_set<std::string> dummy_active_ids_;
};

} // namespace backtest
