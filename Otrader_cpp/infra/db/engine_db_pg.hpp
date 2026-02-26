#pragma once

/**
 * DatabaseEngine (live): PostgreSQL contract/order/trade 持久化（libpqxx）。
 * load_contracts() 按固定顺序发出 Contract 事件，由 EventEngine 派发至
 * MarketDataEngine::process_contract， 建立 portfolio 结构；后续 Snapshot 事件经 dispatch_snapshot
 * → apply_frame 更新行情/Greeks。
 */

#include "../../core/engine_log.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/constant.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pqxx {
class connection;
}

namespace engines {

struct MainEngine;

class DatabaseEngine : public utilities::BaseEngine {
  public:
    /** conninfo: PostgreSQL connection string (e.g. "dbname=trading" or env DATABASE_URL). */
    explicit DatabaseEngine(utilities::MainEngine* main_engine, const std::string& conninfo = "");
    ~DatabaseEngine() override;

    void load_contracts();
    void save_contract_data(const utilities::ContractData& contract, const std::string& symbol_key);
    std::unordered_map<std::string, utilities::ContractData>
    load_contract_data(const std::string* symbol_key = nullptr);

    void save_order_data(const std::string& strategy_name, const utilities::OrderData& order);
    void save_trade_data(const std::string& strategy_name, const utilities::TradeData& trade);
    std::vector<std::vector<std::string>> get_all_history_orders();
    std::vector<std::vector<std::string>> get_all_history_trades();
    void wipe_trading_data();

    void close() override;

  private:
    void create_tables();
    void cleanup_expired_options();
    void write_log(const std::string& msg, int level = INFO);

    std::string conninfo_;
    std::unique_ptr<pqxx::connection> conn_;
    std::mutex db_mutex_;
};

} // namespace engines
