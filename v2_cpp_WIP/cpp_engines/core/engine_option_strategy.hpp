#pragma once

/**
 * OptionStrategyEngine: strategy instances + OMS state; capabilities via RuntimeAPI.
 * Shared by backtest and live; no interface, no current_strategy_name_.
 * RuntimeAPI structure in runtime_api.hpp (Execution/Portfolio/System).
 */

#include "../utilities/base_engine.hpp"
#include "../utilities/constant.hpp"
#include "../utilities/event.hpp"
#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"
#include "runtime_api.hpp"
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace strategy_cpp {
class OptionStrategyTemplate;
}

namespace core {

class OptionStrategyEngine : public utilities::BaseEngine {
  public:
    OptionStrategyEngine(utilities::MainEngine* main, RuntimeAPI api);
    ~OptionStrategyEngine() override;

    /** Load strategy defaults from Otrader/strategy_config.json. */
    void load_strategy_config();

    void process_order(utilities::OrderData& order);
    void process_trade(const utilities::TradeData& trade);

    strategy_cpp::OptionStrategyTemplate* get_strategy(const std::string& strategy_name);
    /** Returns single strategy (e.g. backtest) or nullptr. */
    strategy_cpp::OptionStrategyTemplate* get_strategy();
    utilities::StrategyHolding* get_strategy_holding(const std::string& strategy_name) const;
    /** Returns holding of single strategy. */
    utilities::StrategyHolding* get_strategy_holding() const;

    utilities::PortfolioData* get_portfolio(const std::string& portfolio_name) const;
    utilities::StrategyHolding* get_holding(const std::string& strategy_name) const;
    const utilities::ContractData* get_contract(const std::string& symbol) const;
    void write_log(const std::string& msg, int level = 0) const;
    void write_log(const utilities::LogData& log) const;

    /** Send order with explicit strategy_name. */
    std::string send_order(const std::string& strategy_name,
                           const utilities::OrderRequest& req) const;
    /** Convenience: build OrderRequest from symbol, call send_order. */
    std::vector<std::string>
    send_order(const std::string& strategy_name, const std::string& symbol,
               utilities::Direction direction, double price, double volume,
               utilities::OrderType order_type = utilities::OrderType::LIMIT);
    std::vector<std::string>
    send_combo_order(const std::string& strategy_name, utilities::ComboType combo_type,
                     const std::string& combo_sig, utilities::Direction direction, double price,
                     double volume, std::span<const utilities::Leg> legs,
                     utilities::OrderType order_type = utilities::OrderType::LIMIT);

    void add_strategy(const std::string& class_name, const std::string& portfolio_name,
                      const std::unordered_map<std::string, double>& setting = {});

    /** Default params for strategy class (strategy_config.json). */
    std::unordered_map<std::string, double>
    get_default_setting(const std::string& class_name) const;

    void init_strategy(const std::string& strategy_name);
    void start_strategy(const std::string& strategy_name);
    void stop_strategy(const std::string& strategy_name);
    /** Remove strategy and clear holding; returns success. */
    bool remove_strategy(const std::string& strategy_name);

    /** Call on_timer for all strategies (event-driven). */
    void on_timer();
    utilities::OrderData* get_order(const std::string& orderid) const;
    utilities::TradeData* get_trade(const std::string& tradeid) const;
    /** Get strategy_name by orderid for save_order_data. */
    std::string get_strategy_name_for_order(const std::string& orderid) const;
    std::vector<utilities::OrderData> get_all_orders() const;
    std::vector<utilities::TradeData> get_all_trades() const;
    std::vector<utilities::OrderData> get_all_active_orders() const;
    const std::unordered_map<std::string, std::set<std::string>>&
    get_strategy_active_orders() const;
    /** Loaded strategy names (for hedge iteration). */
    std::vector<std::string> get_strategy_names() const;

    void close() override;

    engines::ComboBuilderEngine* combo_builder_engine() const;
    engines::HedgeEngine* hedge_engine() const;

    /** Backtest: insert orderid after append_order. */
    std::unordered_set<std::string>& active_order_ids() {
        return api_.execution.get_active_order_ids ? api_.execution.get_active_order_ids()
                                                   : dummy_active_order_ids_;
    }
    /** Remove orderid tracking on cancel. */
    void remove_order_tracking(const std::string& orderid) const;

  private:
    std::unordered_map<std::string, double>
    load_strategy_defaults(const std::string& class_name) const;

    RuntimeAPI api_;
    bool strategy_config_loaded_ = false;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> strategy_defaults_;
    std::unordered_map<std::string, std::unique_ptr<strategy_cpp::OptionStrategyTemplate>>
        strategies_;
    std::unordered_set<std::string> dummy_active_order_ids_;

    // Order assembly
    bool assemble_order_request(const std::string& strategy_name, const std::string& symbol,
                                utilities::Direction direction, double price, double volume,
                                utilities::OrderType order_type,
                                std::span<const utilities::Leg> legs,
                                std::optional<utilities::ComboType> combo_type,
                                const std::string* combo_sig,
                                utilities::OrderRequest& out_req) const;
};

} // namespace core
