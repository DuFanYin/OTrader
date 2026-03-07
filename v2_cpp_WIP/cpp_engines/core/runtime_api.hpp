#pragma once

/**
 * Runtime API: runtime capabilities split into three blocks for core/strategy.
 * - ExecutionAPI: send order, order/trade status, cancel tracking (ExecutionEngine/Main)
 * - PortfolioAPI: portfolio, contract, strategy holding (Main + PositionEngine)
 * - SystemAPI: events, logs, helper engines (Hedge/ComboBuilder)
 * Structure defined here; populated in MainEngine constructors.
 */

#include "../utilities/constant.hpp"
#include "../utilities/event.hpp"
#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"

#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engines {
class ComboBuilderEngine;
class HedgeEngine;
} // namespace engines

namespace core {

struct ExecutionAPI {
    // Send order: strategy + request → orderid
    std::function<std::string(const std::string& strategy_name, const utilities::OrderRequest& req)>
        send_order;

    // Cancel order
    std::function<void(const utilities::CancelRequest&)> cancel_order;

    // Order/trade query and iteration
    std::function<utilities::OrderData*(const std::string&)> get_order;
    std::function<utilities::TradeData*(const std::string&)> get_trade;
    std::function<std::string(const std::string&)> get_strategy_name_for_order;
    std::function<std::vector<utilities::OrderData>()> get_all_orders;
    std::function<std::vector<utilities::TradeData>()> get_all_trades;
    std::function<std::vector<utilities::OrderData>()> get_all_active_orders;
    std::function<const std::unordered_map<std::string, std::set<std::string>>&()>
        get_strategy_active_orders;

    // Cleanup on cancel/strategy remove
    std::function<void(const std::string&)> remove_order_tracking;
    std::function<std::unordered_set<std::string>&()> get_active_order_ids;
    std::function<void(const std::string&)> ensure_strategy_key;
    std::function<void(const std::string&)> remove_strategy_tracking;
};

struct PortfolioAPI {
    // Portfolio / contract / strategy holding
    std::function<utilities::PortfolioData*(const std::string&)> get_portfolio;
    std::function<const utilities::ContractData*(const std::string&)> get_contract;
    std::function<utilities::StrategyHolding*(const std::string&)> get_holding;

    // Holding management for strategy lifecycle
    std::function<void(const std::string& strategy_name)> get_or_create_holding;
    std::function<void(const std::string& strategy_name)> remove_strategy_holding;
};

struct SystemAPI {
    // Log and strategy events
    std::function<void(const utilities::LogData&)> write_log;
    std::function<void(const utilities::StrategyUpdateData&)> put_strategy_event;

    // Helper engines
    std::function<engines::ComboBuilderEngine*()> get_combo_builder_engine;
    std::function<engines::HedgeEngine*()> get_hedge_engine;
};

struct RuntimeAPI {
    ExecutionAPI execution;
    PortfolioAPI portfolio;
    SystemAPI system;
};

} // namespace core
