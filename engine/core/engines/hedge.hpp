#pragma once

/** HedgeEngine: delta hedging, each registered strategy's holding back into its delta range. */

#include "utilities/base_engine.hpp"
#include "model/constant.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "core/runtime_api.hpp"
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace model {
class PortfolioStructure;
} // namespace model

namespace core {

class ExecutionEngine;
class PositionEngine;

/** Per-strategy config (Python HedgeConfig). */
struct HedgeConfig {
    std::string strategy_name;
    std::string portfolio_name;
    int timer_trigger = 5; // hedge every timer_trigger bars
    int delta_target = 0;
    int delta_range = 0;
    model::BarGate bar_gate; // one look per bar
    int timer_cnt = 0;           // bars since the last hedge
};

class HedgeEngine : public utilities::BaseEngine {
  public:
    /** Reads the portfolios (their contracts), the holdings and the orders working; sends and
     * cancels through runtime. */
    HedgeEngine(utilities::Logger* logger, model::PortfolioStructure& portfolios,
                PositionEngine& positions, ExecutionEngine& execution, RuntimeAPI& runtime);

    void register_strategy(const std::string& strategy_name, const std::string& portfolio_name,
                           int timer_trigger = 5, int delta_target = 0, int delta_range = 0);
    void unregister_strategy(const std::string& strategy_name);

    /** The registered strategy's hedge now, its holding valued in this portfolio: the orders to
     * send, or the cancels of its hedge still working (one is never stacked on another). */
    void process_hedging(const std::string& strategy_name, const model::PortfolioData& portfolio,
                         const model::StrategyHolding& holding,
                         std::vector<model::OrderRequest>* out_orders,
                         std::vector<model::CancelRequest>* out_cancels);

    /** Hedge each registered strategy of these, in this order, once per bar (every timer_trigger
     * bars), as a strategy's on_timer_logic runs. */
    void process_timer_event(const std::vector<std::string>& strategy_names);

    const std::unordered_map<std::string, HedgeConfig>& registered_strategies() const {
        return registered_strategies_;
    }

  private:
    void run_strategy_hedging(const std::string& strategy_name, const HedgeConfig& config,
                              const model::PortfolioData& portfolio,
                              const model::StrategyHolding& holding,
                              std::vector<model::OrderRequest>* out_orders,
                              std::vector<model::CancelRequest>* out_cancels);
    std::optional<std::tuple<std::string, model::Direction, double, double>>
    compute_hedge_plan(const HedgeConfig& config, const model::PortfolioData& portfolio,
                       const model::StrategyHolding& holding) const;
    void execute_hedge_orders(const std::string& strategy_name, const std::string& symbol,
                              model::Direction direction, double available, double order_volume,
                              std::vector<model::OrderRequest>* out_orders);
    void submit_hedge_order(const std::string& strategy_name, const std::string& symbol,
                            model::Direction direction, double volume,
                            std::vector<model::OrderRequest>* out_orders);
    /** The strategy's hedge orders still working. */
    std::vector<const model::OrderData*> working_hedges(const std::string& strategy_name) const;

    model::PortfolioStructure& portfolios_;
    PositionEngine& positions_;
    ExecutionEngine& execution_;
    RuntimeAPI& runtime_;
    std::unordered_map<std::string, HedgeConfig> registered_strategies_;
};

} // namespace core
