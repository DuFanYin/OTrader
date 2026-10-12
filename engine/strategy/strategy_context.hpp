#pragma once

/**
 * StrategyContext: what a strategy sees of the engine that runs it, and what it may ask of it.
 * The strategy engine (core::OptionStrategyEngine) implements it, in backtest and live alike, and a
 * test can implement it to run a strategy alone. Strategies depend on this, never on the engine.
 */

#include "../model/constant.hpp"
#include "../utilities/log_level.hpp"
#include "../model/object.hpp"
#include "../model/portfolio.hpp"
#include <span>
#include <string>
#include <vector>

namespace strategy {

class StrategyContext {
  public:
    virtual ~StrategyContext() = default;

    // ── What a strategy sees

    virtual const model::PortfolioData*
    get_portfolio(const std::string& portfolio_name) const = 0;
    virtual const model::ContractData* get_contract(const std::string& symbol) const = 0;
    /** True while an order the strategy sent is still working. */
    virtual bool has_active_order(const std::string& strategy_name) const = 0;

    // ── What it may do (each returns the ids of the orders it sent; none on a refusal)

    virtual std::vector<std::string> send_order(const std::string& strategy_name,
                                                const std::string& symbol,
                                                model::Direction direction, double price,
                                                double volume, model::OrderType order_type) = 0;
    virtual std::vector<std::string>
    send_combo_order(const std::string& strategy_name, model::ComboType combo_type,
                     const std::string& combo_sig, model::Direction direction, double price,
                     double volume, std::span<const model::Leg> legs,
                     model::OrderType order_type) = 0;
    /** Keep the strategy's delta near delta_target (within delta_range), every timer_trigger
     * bars. */
    virtual void register_hedging(const std::string& strategy_name,
                                  const std::string& portfolio_name, int timer_trigger,
                                  int delta_target, int delta_range) = 0;
    virtual void unregister_hedging(const std::string& strategy_name) = 0;
    /** Live quotes for these chains, until the strategy is removed. */
    virtual void subscribe_chains(const std::string& strategy_name,
                                  std::span<const std::string> chains) = 0;
    /** Log msg as the strategy. */
    virtual void write_log(const std::string& strategy_name, const std::string& msg,
                           utilities::LogLevel level) const = 0;
};

} // namespace strategy
