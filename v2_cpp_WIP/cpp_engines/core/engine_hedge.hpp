#pragma once

/** HedgeEngine: delta hedging; returns intents (orders/cancels/logs). */

#include "../utilities/base_engine.hpp"
#include "../utilities/constant.hpp"
#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace engines {

/** Read-only params for hedging (no execution callbacks). */
struct HedgeParams {
    utilities::PortfolioData* portfolio = nullptr;
    utilities::StrategyHolding* holding = nullptr;
    std::function<const utilities::ContractData*(const std::string&)> get_contract;
    std::function<const std::unordered_map<std::string, std::set<std::string>>&()>
        get_strategy_active_orders;
    std::function<utilities::OrderData*(const std::string&)> get_order;
};

/** Per-strategy config (Python HedgeConfig). */
struct HedgeConfig {
    std::string strategy_name;
    int timer_trigger = 5;
    int delta_target = 0;
    int delta_range = 0;
};

class HedgeEngine : public utilities::BaseEngine {
  public:
    HedgeEngine() = default;
    explicit HedgeEngine(utilities::MainEngine* main) : BaseEngine(main, "Hedge") {}

    void register_strategy(const std::string& strategy_name, int timer_trigger = 5,
                           int delta_target = 0, int delta_range = 0);
    void unregister_strategy(const std::string& strategy_name);

    /** Run hedging; append OrderRequest/CancelRequest/LogData to caller vectors. */
    void process_hedging(const std::string& strategy_name, const HedgeParams& params,
                         std::vector<utilities::OrderRequest>* out_orders,
                         std::vector<utilities::CancelRequest>* out_cancels,
                         std::vector<utilities::LogData>* out_logs);

    const std::unordered_map<std::string, HedgeConfig>& registered_strategies() const {
        return registered_strategies_;
    }

  private:
    static void run_strategy_hedging_with_params(const std::string& strategy_name,
                                                 HedgeConfig& config, const HedgeParams& params,
                                                 std::vector<utilities::OrderRequest>* out_orders,
                                                 std::vector<utilities::CancelRequest>* out_cancels,
                                                 std::vector<utilities::LogData>* out_logs);
    static std::optional<std::tuple<std::string, utilities::Direction, double, double>>
    compute_hedge_plan(const std::string& strategy_name, HedgeConfig& config,
                       const HedgeParams& params);
    static void execute_hedge_orders(const std::string& strategy_name, const std::string& symbol,
                                     utilities::Direction direction, double available,
                                     double order_volume, const HedgeParams& params,
                                     std::vector<utilities::OrderRequest>* out_orders,
                                     std::vector<utilities::LogData>* out_logs);
    static void submit_hedge_order(const std::string& strategy_name, const std::string& symbol,
                                   utilities::Direction direction, double volume,
                                   const HedgeParams& params,
                                   std::vector<utilities::OrderRequest>* out_orders,
                                   std::vector<utilities::LogData>* out_logs);
    static bool check_strategy_orders_finished(const std::string& strategy_name,
                                               const HedgeParams& params);
    static void cancel_strategy_orders(const std::string& strategy_name, const HedgeParams& params,
                                       std::vector<utilities::CancelRequest>* out_cancels);

    std::unordered_map<std::string, HedgeConfig> registered_strategies_;
};

} // namespace engines
