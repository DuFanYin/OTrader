#pragma once

/**
 * DatabaseEngine (live): PostgreSQL contract/order/trade 持久化（libpqxx）。
 * load_contracts(apply_option, apply_underlying) 两段 load，load 后调用方 finalize_all_chains。
 */

#include "../../core/engine_log.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/constant.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include <functional>
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

    /** Option table → apply_option; equity table → apply_underlying. Blocking, no event pipeline.
     */
    void
    load_contracts(const std::function<void(const utilities::ContractData&)>& apply_option,
                   const std::function<void(const utilities::ContractData&)>& apply_underlying);

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
    std::unordered_map<std::string, utilities::ContractData>
    load_option_contract_data(const std::string* symbol_key);
    std::unordered_map<std::string, utilities::ContractData>
    load_equity_contract_data(const std::string* symbol_key);

    std::string conninfo_;
    std::unique_ptr<pqxx::connection> conn_;
    std::mutex db_mutex_;
};

} // namespace engines
