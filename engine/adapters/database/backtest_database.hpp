#pragma once

/**
 * core::Database for a backtest, in memory: what a run asks back (its strategies, its settings, the
 * names used). Orders and fills, which a backtest never reads back, leave only their strategy's
 * name, so it does not grow with the days.
 */

#include "core/database/database.hpp"
#include <mutex>
#include <ranges>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace adapters {

class BacktestDatabase final : public core::Database {
  public:
    std::vector<model::Contract> load_option_contracts() override { return {}; }
    std::vector<model::Contract> load_equity_contracts() override { return {}; }
    void save_contracts(std::span<const model::Contract> /*contracts*/) override {}

    void save_order(const std::string& strategy_name,
                    const model::Order& /*order*/) override {
        std::scoped_lock lock(mutex_);
        names_.insert(strategy_name);
    }
    void save_fill(const std::string& strategy_name,
                   const model::Fill& /*fill*/) override {
        std::scoped_lock lock(mutex_);
        names_.insert(strategy_name);
    }
    std::vector<core::SavedOrder> load_orders(const std::vector<std::string>& /*strategies*/,
                                              std::size_t /*latest*/) override {
        return {};
    }
    std::vector<core::SavedFill> load_fills(const std::vector<std::string>& /*strategies*/,
                                            std::size_t /*latest*/) override {
        return {};
    }
    std::vector<std::string> strategies_with_records() override {
        std::scoped_lock lock(mutex_);
        return {names_.begin(), names_.end()};
    }

    void save_setting(const std::string& name, const std::string& value) override {
        std::scoped_lock lock(mutex_);
        settings_[name] = value;
    }
    std::unordered_map<std::string, std::string> load_settings() override {
        std::scoped_lock lock(mutex_);
        return settings_;
    }

    std::vector<std::pair<std::string, int>> delete_orders_and_fills() override {
        std::scoped_lock lock(mutex_);
        names_.clear();
        return {};
    }
    std::vector<std::pair<std::string, int>> delete_contracts() override { return {}; }

    void save_strategy(const model::StrategyRecord& record) override {
        std::scoped_lock lock(mutex_);
        strategies_[record.name] = record;
    }
    void delete_strategy(const std::string& name) override {
        std::scoped_lock lock(mutex_);
        strategies_.erase(name);
    }
    std::vector<model::StrategyRecord> load_strategies() override {
        std::scoped_lock lock(mutex_);
        return strategies_ | std::views::values | std::ranges::to<std::vector>();
    }
    bool is_strategy_name_used(const std::string& name) override {
        std::scoped_lock lock(mutex_);
        return strategies_.contains(name) || names_.contains(name);
    }

    void close() override {}

  private:
    std::mutex mutex_;
    std::set<std::string> names_; // strategies with an order or a fill saved
    std::unordered_map<std::string, std::string> settings_;
    std::unordered_map<std::string, model::StrategyRecord> strategies_;
};

} // namespace adapters
