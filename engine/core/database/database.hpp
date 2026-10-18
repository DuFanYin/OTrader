#pragma once

/**
 * Database: the storage port (contracts, orders, fills, settings, strategies). Thread-safe. Orders
 * and fills are queued and batch-written so the event thread never waits on disk; reads see all
 * earlier saves. A store that failed to open is a no-op.
 */

#include "model/object.hpp"
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace core {

/** An order or a fill as saved: the strategy it is for, and when it was saved. */
template <typename Record> struct Saved {
    std::string strategy_name;
    std::chrono::system_clock::time_point saved_at;
    Record record;
};
using SavedOrder = Saved<model::Order>;
using SavedFill = Saved<model::Fill>;
/** Either, as a write queue holds them. */
using SavedRecord = Saved<std::variant<model::Order, model::Fill>>;

class Database {
  public:
    virtual ~Database() = default;

    virtual std::vector<model::Contract> load_option_contracts() = 0;
    virtual std::vector<model::Contract> load_equity_contracts() = 0;
    /** One transaction, replacing same-symbol contracts; throws if refused. */
    virtual void save_contracts(std::span<const model::Contract> contracts) = 0;

    /** Queued, returns at once. An order's latest state replaces earlier ones. */
    virtual void save_order(const std::string& strategy_name,
                            const model::Order& order) = 0;
    virtual void save_fill(const std::string& strategy_name,
                           const model::Fill& fill) = 0;
    /** In save order; empty strategies means all; latest 0 means all. */
    virtual std::vector<SavedOrder> load_orders(const std::vector<std::string>& strategies = {},
                                                std::size_t latest = 0) = 0;
    virtual std::vector<SavedFill> load_fills(const std::vector<std::string>& strategies = {},
                                              std::size_t latest = 0) = 0;
    virtual std::vector<std::string> strategies_with_records() = 0;

    /** Engine settings (risk limits, halt). */
    virtual void save_setting(const std::string& name, const std::string& value) = 0;
    virtual std::unordered_map<std::string, std::string> load_settings() = 0;

    /** Returns rows deleted per table; also drops queued writes. Throws if refused. */
    virtual std::vector<std::pair<std::string, int>> delete_orders_and_fills() = 0;
    virtual std::vector<std::pair<std::string, int>> delete_contracts() = 0;

    /** Upsert by name, on each change of its state. A failure is only logged; a restart then will
     * not know the strategy as it was. */
    virtual void save_strategy(const model::StrategyRecord& record) = 0;
    /** Its orders and fills stay. */
    virtual void delete_strategy(const std::string& name) = 0;
    virtual std::vector<model::StrategyRecord> load_strategies() = 0;
    /** Names are never reused, since history is looked up by name: true if kept or if any order or
     * fill (queued included) has it. Throws when the store cannot be read. */
    virtual bool is_strategy_name_used(const std::string& name) = 0;

    virtual void close() = 0;
};

} // namespace core
