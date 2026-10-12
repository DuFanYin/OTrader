#pragma once

/**
 * DatabaseEngine (live): contracts, strategies, orders and trades in one SQLite file
 * ($OPTRA_DB, default data/optra.db). load_contracts(apply_option, apply_underlying)
 * two-phase load; caller finalize_all_chains after. Orders and trades are written by a
 * thread of their own, in batches (every kWriteEvery, or at once when kWriteBatch are queued): the
 * event thread only queues them, and wakes nothing for one. A read writes what is queued first, so
 * it sees every record saved before it. A restart reads the strategies, orders and trades back to
 * resume where the engine stopped.
 */

#include "../../utilities/base_engine.hpp"
#include "../../utilities/log_level.hpp"
#include "../../model/object.hpp"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

struct sqlite3;

namespace adapters {

/** What the operator last asked of a strategy: a restart brings it back to that. */
enum class StrategyState : std::uint8_t { Added, Inited, Running, Removed };

std::string to_string(StrategyState state);

/** A strategy as kept across restarts: enough to add it again as it was. */
struct StrategyRecord {
    std::string name;
    std::string class_name;
    std::string portfolio;
    std::unordered_map<std::string, double> setting; // resolved: every declared parameter
    StrategyState state = StrategyState::Added;
};

/** An order or a fill as saved: the strategy it is for, and when it was saved. */
struct SavedRecord {
    std::string strategy_name;
    std::chrono::system_clock::time_point saved_at;
    std::variant<model::OrderData, model::TradeData> record;
};

/** The text a time is kept in (UTC, to the second), as the UI shows it. */
std::string datetime_to_str(std::chrono::system_clock::time_point tp);

class DatabaseEngine : public utilities::BaseEngine {
  public:
    /** How often the writer writes what is queued, and how many queued wake it sooner. */
    static constexpr std::chrono::milliseconds kWriteEvery{20};
    static constexpr std::size_t kWriteBatch = 256;

    /** path: the SQLite file (empty: $OPTRA_DB, else data/optra.db), created if missing. */
    explicit DatabaseEngine(utilities::Logger* logger, const std::string& path = "");
    ~DatabaseEngine();

    DatabaseEngine(const DatabaseEngine&) = delete;
    DatabaseEngine& operator=(const DatabaseEngine&) = delete;

    /** Option table → apply_option; equity table → apply_underlying. Blocking, no event
     * pipeline. */
    void
    load_contracts(const std::function<void(const model::ContractData&)>& apply_option,
                   const std::function<void(const model::ContractData&)>& apply_underlying);
    /** The contracts saved, options then the rest, as the tables keep them. */
    std::vector<model::ContractData> load_option_contract_data();
    std::vector<model::ContractData> load_equity_contract_data();

    /** Write contracts in one transaction (contract_option for options, contract_equity for the
     * rest), replacing the rows of the same symbols; throws if the file refuses them. */
    void save_contracts(std::span<const model::ContractData> contracts);

    /** Queue the order (or trade) for the writer thread and return: no disk on the event path. */
    void save_order_data(const std::string& strategy_name, const model::OrderData& order);
    void save_trade_data(const std::string& strategy_name, const model::TradeData& trade);

    /** The orders (OrderData records) and the trades (TradeData records) of these strategies, of
     * all for none, in the order saved; the latest `latest` of them (0: all): the UI lists them,
     * a restart books them all again. */
    std::vector<SavedRecord> load_orders(const std::vector<std::string>& strategies = {},
                                         std::size_t latest = 0);
    std::vector<SavedRecord> load_trades(const std::vector<std::string>& strategies = {},
                                         std::size_t latest = 0);
    /** Every strategy name orders or trades are saved under, sorted. */
    std::vector<std::string> strategies_with_records();
    /** The engine's own settings by name (the risk limits, the halt): written at once. */
    void save_setting(const std::string& name, const std::string& value);
    std::unordered_map<std::string, std::string> load_settings();

    /** Empty the orders and trades (what is queued written first), or the contracts;
     * each table with the rows it lost. Throws if the file refuses. */
    std::vector<std::pair<std::string, int>> delete_orders_and_trades();
    std::vector<std::pair<std::string, int>> delete_contracts();

    // ── Strategies, written at once (not queued): they change only when the operator acts

    /** Insert, or replace the record of the same name. A failure is logged: the engine runs on,
     * and a restart will not know the strategy. */
    void save_strategy(const StrategyRecord& record);
    void set_strategy_state(const std::string& name, StrategyState state);
    /** Forget it; its orders and trades stay. */
    void delete_strategy(const std::string& name);
    std::vector<StrategyRecord> load_strategies();
    /** A strategy of this name was added before: it is kept here, or orders or trades are (or
     * are about to be) under its name. A name is a strategy's for good: its history is found by
     * it. Throws when the file cannot be read. */
    bool is_strategy_name_used(const std::string& name);

    /** Write what is queued, stop the writer, close the file. */
    void close();

  private:
    void create_tables();
    /** Add a column a table made by an older version lacks. db_mutex_ held. */
    void ensure_column(const char* table, const char* column, const char* type);
    /** Drop a column an older version kept, when the table has it. db_mutex_ held. */
    void drop_column(const char* table, const char* column);
    void cleanup_expired_options();
    void queue(SavedRecord pending);
    /** The orders (trades) a SELECT_ORDERS (SELECT_TRADES) query finds, ?1.. bound to `values`. */
    std::vector<SavedRecord> read_orders(const std::string& sql,
                                         const std::vector<std::string>& values);
    std::vector<SavedRecord> read_trades(const std::string& sql,
                                         const std::vector<std::string>& values);
    void run_writer(const std::stop_token& st);
    /** Write what is queued in one transaction. db_mutex_ held: what is queued and what is
     * written change together, so emptying a table after it loses nothing queued before. */
    void write_pending();
    /** Empty the tables in one transaction; each with the rows it lost. db_mutex_ held. */
    std::vector<std::pair<std::string, int>> delete_rows(std::span<const char* const> tables);

    std::string path_;
    sqlite3* db_ = nullptr;
    // The connection: the writer thread and the readers. Taken before pending_mutex_.
    std::mutex db_mutex_;

    std::vector<SavedRecord> pending_;
    std::mutex pending_mutex_;
    std::condition_variable_any pending_cv_;
    std::jthread writer_;
};

} // namespace adapters
