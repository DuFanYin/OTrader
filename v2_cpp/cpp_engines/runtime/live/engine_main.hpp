#pragma once

/** MainEngine: holds engines, EventEngine dispatch, accessors. */

#include "../../core/engine_execution.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_log.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../core/engine_position.hpp"
#include "../../core/portfolio_structure.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/mpsc_ring.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/object_pool.hpp"
#include "../../utilities/portfolio.hpp"
#include "engine_db_pg.hpp"
#include "engine_event.hpp"
#include "gateway_client.hpp"
#include "market_data_client.hpp"
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_set>

namespace engines {

class MainEngine : public utilities::MainEngine {
  public:
    MainEngine();
    ~MainEngine() override;

    EventEngine* event_engine() { return event_engine_.get(); }
    LogEngine* log_engine() { return log_engine_.get(); }
    DatabaseEngine* db_engine() { return db_engine_.get(); }
    PortfolioStructure* portfolio_structure() { return portfolio_structure_.get(); }
    GatewayClient* gateway_client() { return gateway_client_.get(); }
    MarketDataClient* market_data_client() { return market_data_client_.get(); }
    core::ExecutionEngine* execution_engine() { return execution_engine_.get(); }
    core::OptionStrategyEngine* option_strategy_engine() { return option_strategy_engine_.get(); }
    PositionEngine* position_engine() { return position_engine_.get(); }
    HedgeEngine* hedge_engine();
    utilities::StrategyHolding* get_holding(const std::string& strategy_name);
    const utilities::StrategyHolding* get_holding(const std::string& strategy_name) const;
    void get_or_create_holding(const std::string& strategy_name);

    void start_market_data_update();
    void stop_market_data_update();
    void subscribe_chains(const std::string& strategy_name,
                          std::span<const std::string> chain_symbols);
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

    utilities::OrderData* get_order(const std::string& orderid);
    utilities::TradeData* get_trade(const std::string& tradeid);

    void put_event(const utilities::Event& e) override;
    void put_event(utilities::Event&& e) override;
    void put_event(utilities::Event& e) override;
    void write_log(const std::string& msg, int level = engines::INFO,
                   const std::string& gateway = "") override;
    /** Execute log intent. */
    void put_log_intent(const utilities::LogData& log);
    /** Pooled log: acquire, fill, put_log_intent(*p), release_log(p). */
    utilities::LogData* acquire_log();
    void release_log(utilities::LogData* p);
    /** Pooled snapshot: acquire, fill, put_event(Event(Snapshot, p)); EventEngine releases on
     * dispatch/drain. */
    utilities::PortfolioSnapshot* acquire_snapshot() override;
    utilities::OrderData* acquire_order() override;
    utilities::TradeData* acquire_trade() override;
    void close();

    bool market_data_running() const { return market_data_running_; }

    /** append_* → send_order/cancel_order/put_log_intent. */
    std::string append_order(const utilities::OrderRequest& req);
    void append_cancel(const utilities::CancelRequest& req);
    void append_log(const utilities::LogData& log);

    /** Log level threshold; DISABLED = off. */
    void set_log_level(int level);
    int log_level() const;

    /** Strategy updates queue (gRPC). */
    void on_strategy_event(const utilities::StrategyUpdateData& update);
    bool pop_strategy_update(utilities::StrategyUpdateData& out, int timeout_ms);

    /** Log queue (gRPC). */
    bool pop_log_for_stream(utilities::LogData& out, int timeout_ms);

  private:
    /** Self-check: strategy count, portfolio name/chains/options. */
    void log_self_check();

    std::unique_ptr<EventEngine> event_engine_;
    std::unique_ptr<LogEngine> log_engine_;
    std::unique_ptr<DatabaseEngine> db_engine_;
    std::unique_ptr<PortfolioStructure> portfolio_structure_;
    std::unique_ptr<MarketDataClient> market_data_client_;
    std::unique_ptr<GatewayClient> gateway_client_;
    std::unique_ptr<core::ExecutionEngine> execution_engine_;
    std::unique_ptr<core::OptionStrategyEngine> option_strategy_engine_;
    std::unique_ptr<PositionEngine> position_engine_;
    std::unique_ptr<HedgeEngine> hedge_engine_;

    static constexpr size_t kStrategyUpdatesRingCap = 256;
    utilities::ObjectPool<utilities::StrategyUpdateData> strategy_updates_pool_;
    utilities::MpscRing<utilities::StrategyUpdateData*, kStrategyUpdatesRingCap>
        strategy_updates_ring_;
    std::mutex strategy_updates_mutex_;
    std::condition_variable strategy_updates_cv_;

    std::unordered_set<std::string> dummy_active_ids_;
    bool market_data_running_ = false;
};

} // namespace engines
