#pragma once

/**
 * core::Database in one SQLite file (WAL, also read by the backend).
 * Orders and fills are queued by the event thread and written in batches by a writer thread; a
 * read flushes the queue first. Errors are logged and return nothing unless core::Database throws.
 */

#include "core/database/database.hpp"
#include "utilities/log_source.hpp"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace adapters {

class SqliteDatabase final : public core::Database {
  public:
    /** How often the writer writes what is queued, and how many queued wake it sooner. */
    static constexpr std::chrono::milliseconds kWriteEvery{20};
    static constexpr std::size_t kWriteBatch = 256;

    /** Empty path: $OPTRA_DB, else data/optra.db. Migrates old schemas, drops expired options. */
    explicit SqliteDatabase(utilities::Logger* logger, const std::string& path = "");
    ~SqliteDatabase() override;

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    std::vector<model::Contract> load_option_contracts() override;
    std::vector<model::Contract> load_equity_contracts() override;
    void save_contracts(std::span<const model::Contract> contracts) override;

    void save_order(const std::string& strategy_name, const model::Order& order) override;
    void save_fill(const std::string& strategy_name, const model::Fill& fill) override;
    std::vector<core::SavedOrder> load_orders(const std::vector<std::string>& strategies,
                                               std::size_t latest) override;
    std::vector<core::SavedFill> load_fills(const std::vector<std::string>& strategies,
                                              std::size_t latest) override;
    std::vector<std::string> strategies_with_records() override;

    void save_setting(const std::string& name, const std::string& value) override;
    std::unordered_map<std::string, std::string> load_settings() override;

    std::vector<std::pair<std::string, int>> delete_orders_and_fills() override;
    std::vector<std::pair<std::string, int>> delete_contracts() override;

    void save_strategy(const model::StrategyRecord& record) override;
    void delete_strategy(const std::string& name) override;
    std::vector<model::StrategyRecord> load_strategies() override;
    bool is_strategy_name_used(const std::string& name) override;

    /** Write what is queued, stop the writer, close the file. */
    void close() override;

  private:
    void create_tables();
    void cleanup_expired_options();
    void queue(core::SavedRecord pending);
    /** The rows `sql` finds, ?1.. bound to `values`, each decoded from its statement; `what` names
     * them in the error logged. */
    template <class Decode>
    auto read_records(const std::string& sql, const std::vector<std::string>& values,
                      const char* what, Decode decode)
        -> std::vector<std::invoke_result_t<Decode, sqlite3_stmt*>>;
    void run_writer(const std::stop_token& st);
    /** Write what is queued in one transaction. db_mutex_ held: what is queued and what is
     * written change together, so emptying a table after it loses nothing queued before. */
    void write_pending();
    /** Empty the tables in one transaction; each with the rows it lost. db_mutex_ held. */
    std::vector<std::pair<std::string, int>> delete_rows(std::span<const char* const> tables);

    utilities::LogSource log_;
    std::string path_;
    sqlite3* db_ = nullptr;
    // The connection: the writer thread and the readers. Taken before pending_mutex_.
    std::mutex db_mutex_;

    std::vector<core::SavedRecord> pending_;
    std::vector<core::SavedRecord> writing_; // the batch being written; db_mutex_
    std::mutex pending_mutex_;
    std::condition_variable_any pending_cv_;
    std::jthread writer_;
};

} // namespace adapters
