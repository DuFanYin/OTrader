#pragma once

#include "../../infra/db/engine_db_pg.hpp"
#include "../../utilities/mpsc_ring.hpp"
#include "../../utilities/object_pool.hpp"
#include "../main_engine_base.hpp"
#include "engine_event.hpp"
#include "gateway_client.hpp"
#include "market_data_client.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engines {

class MainEngine : public runtime_common::MainEngineBase {
  public:
    MainEngine();
    ~MainEngine() override;

    EventEngine* event_engine() { return event_engine_.get(); }
    DatabaseEngine* db_engine() { return db_engine_.get(); }
    GatewayClient* gateway_client() { return gateway_client_.get(); }
    MarketDataClient* market_data_client() { return market_data_client_.get(); }

    void start_market_data_update();
    void stop_market_data_update();
    void subscribe_chains(const std::string& strategy_name,
                          std::span<const std::string> chain_symbols);
    void unsubscribe_chains(const std::string& strategy_name);

    std::vector<std::string> get_all_portfolio_names() const;
    std::vector<utilities::ContractData> get_all_contracts() const;
    std::vector<std::pair<std::string, std::string>> get_strategy_errors() const;

    void connect();
    void disconnect();
    void cancel_order(const utilities::CancelRequest& req) override;
    std::string send_order(const utilities::OrderRequest& req);
    std::string send_order(const std::string& strategy_name,
                           const utilities::OrderRequest& req) override;
    void query_account();
    void query_position();

    utilities::TradeData* get_trade(const std::string& tradeid);

    void put_event(const utilities::Event& e) override;
    void put_event(utilities::Event&& e) override;

    utilities::PortfolioSnapshot* acquire_snapshot() override;
    utilities::OrderData* acquire_order() override;
    utilities::TradeData* acquire_trade() override;

    bool market_data_running() const { return market_data_running_; }

    void on_strategy_event(const utilities::StrategyUpdateData& update);
    bool pop_strategy_update(utilities::StrategyUpdateData& out, int timeout_ms);
    bool pop_log_for_stream(utilities::LogData& out, int timeout_ms);

    // ---- Infra hooks ----
    std::string send_order_to_gateway(const utilities::OrderRequest& req) override;
    void save_order_data(const std::string& strategy_name,
                         const utilities::OrderData& order) override;
    void save_trade_data(const std::string& strategy_name,
                         const utilities::TradeData& trade) override;
    void close_infra() override;

  private:
    void log_self_check();

    std::unique_ptr<EventEngine> event_engine_;
    std::unique_ptr<DatabaseEngine> db_engine_;
    std::unique_ptr<MarketDataClient> market_data_client_;
    std::unique_ptr<GatewayClient> gateway_client_;

    static constexpr size_t kStrategyUpdatesRingCap = 256;
    utilities::ObjectPool<utilities::StrategyUpdateData> strategy_updates_pool_;
    utilities::MpscRing<utilities::StrategyUpdateData*, kStrategyUpdatesRingCap>
        strategy_updates_ring_;
    std::mutex strategy_updates_mutex_;
    std::condition_variable strategy_updates_cv_;

    bool market_data_running_ = false;
};

} // namespace engines
