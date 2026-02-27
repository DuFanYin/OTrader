#pragma once

/**
 * MainEngine = 持有各引擎 + 提供 accessor + 执行（send_order/cancel_order/put_log_intent）。
 * 不包含 dispatch 控制逻辑；EventEngine 负责 dispatch，通过 Main 的 accessor 访问各引擎。
 */

#include "../../core/engine_combo_builder.hpp"
#include "../../core/engine_execution.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_log.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../core/engine_position.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/portfolio.hpp"
#include "engine_data_tradier.hpp"
#include "engine_db_pg.hpp"
#include "engine_event.hpp"
#include "engine_gateway_ib.hpp"
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace engines {

class MainEngine : public utilities::MainEngine {
  public:
    explicit MainEngine(EventEngine* event_engine = nullptr);
    ~MainEngine() override;

    EventEngine* event_engine() { return event_engine_ptr_; }
    LogEngine* log_engine() { return log_engine_.get(); }
    DatabaseEngine* db_engine() { return db_engine_.get(); }
    MarketDataEngine* market_data_engine() { return market_data_engine_.get(); }
    IbGateway* ib_gateway() { return ib_gateway_.get(); }
    core::ExecutionEngine* execution_engine() { return execution_engine_.get(); }
    core::OptionStrategyEngine* option_strategy_engine() { return option_strategy_engine_.get(); }
    PositionEngine* position_engine() { return position_engine_.get(); }
    HedgeEngine* hedge_engine();
    ComboBuilderEngine* combo_builder_engine();
    utilities::StrategyHolding* get_holding(const std::string& strategy_name);
    const utilities::StrategyHolding* get_holding(const std::string& strategy_name) const;
    void get_or_create_holding(const std::string& strategy_name);

    void start_market_data_update();
    void stop_market_data_update();
    void subscribe_chains(const std::string& strategy_name,
                          const std::vector<std::string>& chain_symbols);
    void unsubscribe_chains(const std::string& strategy_name);
    utilities::PortfolioData* get_portfolio(const std::string& portfolio_name);
    std::vector<std::string> get_all_portfolio_names() const;
    const utilities::ContractData* get_contract(const std::string& symbol) const;
    std::vector<utilities::ContractData> get_all_contracts() const;

    void save_trade_data(const std::string& strategy_name, const utilities::TradeData& trade);
    void save_order_data(const std::string& strategy_name, const utilities::OrderData& order);

    void connect();
    void disconnect();
    void cancel_order(const utilities::CancelRequest& req);
    std::string send_order(const utilities::OrderRequest& req);
    void query_account();
    void query_position();
    void query_portfolio(const std::string& portfolio_name);

    utilities::OrderData* get_order(const std::string& orderid);
    utilities::TradeData* get_trade(const std::string& tradeid);

    void put_event(const utilities::Event& e) override;
    void write_log(const std::string& msg, int level = engines::INFO,
                   const std::string& gateway = "") override;
    /** Execute log intent (called by EventEngine and others). */
    void put_log_intent(const utilities::LogData& log);
    void close();

    bool market_data_running() const { return market_data_running_; }

    /** Strategy/Hedge 只调 append_*，不持 event_engine；内部直接转
     * send_order/cancel_order/put_log_intent。 */
    std::string append_order(const utilities::OrderRequest& req);
    void append_cancel(const utilities::CancelRequest& req);
    void append_log(const utilities::LogData& log);

    /** Log level: only messages with level >= this are output. Use DISABLED to turn off. */
    void set_log_level(int level);
    int log_level() const;

    /** Strategy updates queue for gRPC StreamStrategyUpdates. */
    void on_strategy_event(const utilities::StrategyUpdateData& update);
    bool pop_strategy_update(utilities::StrategyUpdateData& out, int timeout_ms);

    /** Log queue for gRPC StreamLogs. */
    bool pop_log_for_stream(utilities::LogData& out, int timeout_ms);

  private:
    std::unique_ptr<EventEngine> event_engine_;
    EventEngine* event_engine_ptr_ = nullptr; // always the one in use (owned or external)
    std::unique_ptr<LogEngine> log_engine_;
    std::unique_ptr<DatabaseEngine> db_engine_;
    std::unique_ptr<MarketDataEngine> market_data_engine_;
    std::unique_ptr<IbGateway> ib_gateway_;
    std::unique_ptr<core::ExecutionEngine> execution_engine_;
    std::unique_ptr<core::OptionStrategyEngine> option_strategy_engine_;
    std::unique_ptr<PositionEngine> position_engine_;
    std::unique_ptr<HedgeEngine> hedge_engine_;
    std::unique_ptr<ComboBuilderEngine> combo_builder_engine_;

    std::deque<utilities::StrategyUpdateData> strategy_updates_;
    std::mutex strategy_updates_mutex_;
    std::condition_variable strategy_updates_cv_;

    std::unordered_set<std::string> dummy_active_ids_;
    bool market_data_running_ = false;
};

} // namespace engines
