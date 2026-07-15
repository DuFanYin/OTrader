#pragma once

#include "../core/engine_execution.hpp"
#include "../core/engine_hedge.hpp"
#include "../core/engine_log.hpp"
#include "../core/engine_option_strategy.hpp"
#include "../core/engine_position.hpp"
#include "../core/portfolio_structure.hpp"
#include "../utilities/base_engine.hpp"
#include "../utilities/event.hpp"
#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace runtime_common {

class MainEngineBase {
  public:
    MainEngineBase();
    virtual ~MainEngineBase();

    // ---- Core engine accessors ----
    core::ExecutionEngine* execution_engine() { return execution_engine_.get(); }
    engines::PositionEngine* position_engine() { return position_engine_.get(); }
    engines::LogEngine* log_engine() { return log_engine_.get(); }
    core::OptionStrategyEngine* option_strategy_engine() { return option_strategy_engine_.get(); }
    const core::OptionStrategyEngine* option_strategy_engine() const {
        return option_strategy_engine_.get();
    }
    engines::PortfolioStructure* portfolio_structure() { return portfolio_structure_.get(); }
    const engines::PortfolioStructure* portfolio_structure() const {
        return portfolio_structure_.get();
    }
    engines::HedgeEngine* hedge_engine();

    // ---- MainEngine interface (was utilities::MainEngine) ----
    virtual void write_log(const std::string& msg, int level = engines::INFO,
                           const std::string& gateway = "");
    virtual void put_event(const utilities::Event& e)  = 0;
    virtual void put_event(utilities::Event&& e)       = 0;
    virtual utilities::PortfolioSnapshot* acquire_snapshot() = 0;
    virtual utilities::OrderData*         acquire_order()    = 0;
    virtual utilities::TradeData*         acquire_trade()    = 0;
    virtual std::string send_order(const std::string& strategy_name,
                                   const utilities::OrderRequest& req) {
        (void)strategy_name;
        (void)req;
        return {};
    }
    virtual void cancel_order(const utilities::CancelRequest& /*req*/) {}
    virtual void put_log_intent(const utilities::LogData& log);
    virtual utilities::PortfolioData* get_portfolio(const std::string& name);
    virtual const utilities::ContractData* get_contract(const std::string& symbol) const;
    virtual utilities::StrategyHolding* get_holding(const std::string& strategy_name);
    const utilities::StrategyHolding* get_holding(const std::string& strategy_name) const;
    void get_or_create_holding(const std::string& strategy_name);

    virtual const std::vector<std::string>& get_strategy_names_ref() const;
    virtual const std::unordered_map<std::string, std::set<std::string>>&
    get_strategy_active_orders() const;
    virtual utilities::OrderData* get_order(const std::string& orderid);
    virtual utilities::LogData* acquire_log();
    virtual void release_log(utilities::LogData* p);

    void set_log_level(int level);
    int log_level() const;

    // ---- Infra hooks (subclass must implement) ----
    virtual std::string send_order_to_gateway(const utilities::OrderRequest& req) = 0;
    virtual void close_infra() {}
    virtual void save_order_data(const std::string& /*strategy_name*/,
                                 const utilities::OrderData& /*order*/) {}
    virtual void save_trade_data(const std::string& /*strategy_name*/,
                                 const utilities::TradeData& /*trade*/) {}

    void close();

  protected:
    struct CoreInitParams {
        std::function<std::string(const utilities::OrderRequest&)> send_impl;
        std::function<void(core::ExecutionEngine*, const utilities::CancelRequest&)> cancel_impl;
        std::function<void(const utilities::StrategyUpdateData&)> put_strategy_event;
        int log_level = engines::INFO;
    };

    void init_core(utilities::BaseEngine* event_engine, const CoreInitParams& params);

    std::unique_ptr<engines::PortfolioStructure> portfolio_structure_;
    std::unique_ptr<core::ExecutionEngine> execution_engine_;
    std::unique_ptr<engines::PositionEngine> position_engine_;
    std::unique_ptr<engines::LogEngine> log_engine_;
    std::unique_ptr<core::OptionStrategyEngine> option_strategy_engine_;
    std::unique_ptr<engines::HedgeEngine> hedge_engine_;
    std::unordered_set<std::string> dummy_active_ids_;
};

} // namespace runtime_common
