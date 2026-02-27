#pragma once

/**
 * Shared PositionEngine (from engines/engine_position.py).
 * Tracks strategy holdings, processes order/trade, updates metrics.
 * Caller passes get_portfolio; logs are output as LogIntent (vector<LogData>*).
 */

#include "../utilities/constant.hpp"
#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"
#include "../utilities/utility.hpp"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace engines {

using GetPortfolioFn = std::function<utilities::PortfolioData*(const std::string&)>;

/** Order meta stored when process_order is called (same shape as Python). */
struct OrderMeta {
    bool is_combo = false;
    std::string symbol;
    std::optional<std::string> combo_type;                // ComboType name
    std::vector<std::map<std::string, std::string>> legs; // symbol, con_id, ratio, direction
};

class PositionEngine {
  public:
    PositionEngine() = default;

    /** Caller invokes each tick; pass get_portfolio. Optional out_logs: append LogIntent (LogData)
     * for caller to put_intent_log. */
    void process_timer_event(const GetPortfolioFn& get_portfolio,
                             std::vector<utilities::LogData>* out_logs = nullptr);
    void process_order(const utilities::OrderData& order);
    void process_trade(const std::string& strategy_name, const utilities::TradeData& trade);

    void get_create_strategy_holding(const std::string& strategy_name);
    void remove_strategy_holding(const std::string& strategy_name);
    utilities::StrategyHolding& get_holding(const std::string& strategy_name);

    /** Update metrics; caller passes portfolio from outside. */
    void update_metrics(const std::string& strategy_name, utilities::PortfolioData* portfolio);

    /** Serialize strategy holding to JSON (same shape as Python serialize_holding dict). */
    std::string serialize_holding(const std::string& strategy_name) const;
    /** Load strategy holding from JSON string (same shape as Python load_serialized_holding). */
    void load_serialized_holding(const std::string& strategy_name, const std::string& data);

  private:
    static void apply_underlying_trade(utilities::StrategyHolding& holding,
                                       const utilities::TradeData& trade);
    static void apply_single_leg_option_trade(utilities::StrategyHolding& holding,
                                              const utilities::TradeData& trade);
    static utilities::ComboPositionData*
    get_or_create_combo_position(utilities::StrategyHolding& holding, const std::string& symbol,
                                 utilities::ComboType combo_type,
                                 const std::vector<std::map<std::string, std::string>>* legs_meta);
    static utilities::OptionPositionData*
    get_or_create_option_position(utilities::ComboPositionData& combo,
                                  const utilities::TradeData& trade);
    static void apply_position_change(utilities::ComboPositionData* pos,
                                      const utilities::TradeData& trade);
    static void apply_position_change(utilities::BasePosition* pos,
                                      const utilities::TradeData& trade);

    static std::map<std::string, double>
    accumulate_position(utilities::BasePosition* position,
                        const utilities::OptionData* option_snapshot);
    static std::map<std::string, double>
    accumulate_position(utilities::BasePosition* position,
                        const utilities::UnderlyingData* underlying_snapshot);
    static std::map<std::string, double>
    accumulate_combo_position(utilities::ComboPositionData& combo,
                              utilities::PortfolioData* portfolio);
    static void add_totals(std::map<std::string, double>& totals,
                           const std::map<std::string, double>& metrics);
    static std::string normalize_combo_symbol(const std::string& symbol);

    std::unordered_map<std::string, utilities::StrategyHolding> strategy_holdings_;
    std::unordered_map<std::string, OrderMeta> order_meta_;
    std::set<std::string> trade_seen_;
};

} // namespace engines
