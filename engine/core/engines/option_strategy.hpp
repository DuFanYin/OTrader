#pragma once

/**
 * OptionStrategyEngine: the strategy instances, shared by backtest and live. It reads the
 * portfolios, the holdings (PositionEngine) and the orders (ExecutionEngine) directly, registers
 * hedges with the HedgeEngine, and reaches past the core (orders out, market data, the control
 * plane, the storage) through RuntimeAPI.
 */

#include "strategy/strategy_context.hpp"
#include "utilities/base_engine.hpp"
#include "model/constant.hpp"
#include "model/event.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "core/runtime_api.hpp"
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace strategy {
class OptionStrategyTemplate;
}

namespace model {
class PortfolioStructure;
} // namespace model

namespace core {

class ExecutionEngine;
class HedgeEngine;
class PositionEngine;

/** Hosts the strategies: creates them, drives their callbacks, and is the StrategyContext each one
 * sees the engine through. */
class OptionStrategyEngine : public utilities::BaseEngine, public strategy::StrategyContext {
  public:
    OptionStrategyEngine(utilities::Logger* logger, model::PortfolioStructure& portfolios,
                         ExecutionEngine& execution, PositionEngine& positions,
                         HedgeEngine& hedge, RuntimeAPI& runtime);
    ~OptionStrategyEngine() override;

    void process_order_event(model::OrderData& order);
    void process_trade_event(const model::TradeData& trade);

    strategy::OptionStrategyTemplate* get_strategy(const std::string& strategy_name);
    /** Returns single strategy (e.g. backtest) or nullptr. */
    strategy::OptionStrategyTemplate* get_strategy();
    model::StrategyHolding* get_strategy_holding(const std::string& strategy_name) const;
    /** Returns holding of single strategy. */
    model::StrategyHolding* get_strategy_holding() const;

    // ── StrategyContext: what a strategy sees and may do

    model::PortfolioData* get_portfolio(const std::string& portfolio_name) const override;
    const model::ContractData* get_contract(const std::string& symbol) const override;
    bool has_active_order(const std::string& strategy_name) const override;
    /** Build the OrderRequest for symbol and send it. */
    std::vector<std::string> send_order(const std::string& strategy_name, const std::string& symbol,
                                        model::Direction direction, double price, double volume,
                                        model::OrderType order_type) override;
    std::vector<std::string> send_combo_order(const std::string& strategy_name,
                                              model::ComboType combo_type,
                                              const std::string& combo_sig,
                                              model::Direction direction, double price,
                                              double volume, std::span<const model::Leg> legs,
                                              model::OrderType order_type) override;
    void register_hedging(const std::string& strategy_name, const std::string& portfolio_name,
                          int timer_trigger, int delta_target, int delta_range) override;
    void unregister_hedging(const std::string& strategy_name) override;
    void subscribe_chains(const std::string& strategy_name,
                          std::span<const std::string> chains) override;
    void write_log(const std::string& strategy_name, const std::string& msg,
                   utilities::LogLevel level) const override;
    /** The engine's own log, as "OptionStrategy". */
    using utilities::BaseEngine::write_log;

    // ──

    /** Add a new instance named strategy_name, or for none <class>_<portfolio>, numbered (`_2`,
     * ...) past the names in use or used before; returns the name. A name is a strategy's for
     * good: one in use, or used before (RuntimeAPI::is_strategy_name_used), is refused. Its
     * setting: the class's declared defaults, then `setting`. Throws
     * as well on an unknown class or a setting the class rejects. */
    std::string add_strategy(const std::string& class_name, const std::string& portfolio_name,
                             const std::unordered_map<std::string, double>& setting = {},
                             const std::string& strategy_name = {});
    /** Bring back a strategy from before (a restart, a removed one restored) under its name. */
    void restore_strategy(const std::string& class_name, const std::string& portfolio_name,
                          const std::unordered_map<std::string, double>& setting,
                          const std::string& strategy_name);

    /** A class's setting before overrides: its declared defaults. */
    std::unordered_map<std::string, double>
    get_default_setting(const std::string& class_name) const;
    /** Class of a loaded strategy ("" if none). */
    std::string get_strategy_class_name(const std::string& strategy_name) const;

    void init_strategy(const std::string& strategy_name);
    void start_strategy(const std::string& strategy_name);
    void stop_strategy(const std::string& strategy_name);
    /** Remove the strategy, its holding and what the execution engine keeps of its orders;
     * false when there is none. Refused while it holds positions or has orders working. */
    bool remove_strategy(const std::string& strategy_name);

    /** Call on_timer for all strategies (event-driven). */
    void process_timer_event();
    /** A snapshot of the portfolio was applied: on_snapshot for its strategies that run. */
    void process_snapshot_event(const std::string& portfolio_name);
    /** The strategies' names, in the order they were added. */
    std::vector<std::string> get_strategy_names() const { return strategy_names_; }
    /** The same, without a copy (event thread; changed by add and remove). */
    const std::vector<std::string>& get_strategy_names_ref() const { return strategy_names_; }
    /** Pairs (strategy_name, error_msg) for all strategies with error() == true (e.g. for Live
     * monitoring / gRPC). */
    std::vector<std::pair<std::string, std::string>> get_strategy_errors() const;

    void close();

  private:
    /** Create the instance under `name` (free) and hand it its holding. */
    void create_strategy(const std::string& class_name, const std::string& portfolio_name,
                         const std::unordered_map<std::string, double>& setting,
                         const std::string& name);

    model::PortfolioStructure& portfolios_;
    ExecutionEngine& execution_;
    PositionEngine& positions_;
    HedgeEngine& hedge_;
    RuntimeAPI& runtime_;
    std::unordered_map<std::string, std::unique_ptr<strategy::OptionStrategyTemplate>>
        strategies_;
    std::vector<std::string> strategy_names_; // strategies_'s, in the order added

    // Order assembly
    bool assemble_order_request(const std::string& strategy_name, const std::string& symbol,
                                model::Direction direction, double price, double volume,
                                model::OrderType order_type,
                                std::span<const model::Leg> legs,
                                std::optional<model::ComboType> combo_type,
                                const std::string* combo_sig,
                                model::OrderRequest& out_req) const;
};

} // namespace core
