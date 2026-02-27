#pragma once

/**
 * Runtime API: 将 runtime 能力按职责拆成三块，供 core/strategy 使用。
 *
 * - ExecutionAPI: 下单、订单/成交状态、撤单跟踪等（由 ExecutionEngine/Main 实现）
 * - PortfolioAPI: 组合、合约、策略持仓视图（由 Main + PositionEngine 实现）
 * - SystemAPI: 事件、日志以及辅助引擎（Hedge/ComboBuilder 等）
 *
 * 当前文件只是结构定义，具体填充在各自的 MainEngine 构造函数中完成。
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
    // 发单：策略名 + 请求，返回 orderid（由 ExecutionEngine/Main 注入实现）
    // 单腿与组合单均通过 OrderRequest 表达（is_combo/legs/combo_type 等字段）。
    std::function<std::string(const std::string& strategy_name, const utilities::OrderRequest& req)>
        send_order;

    // 撤单：策略名可由 ExecutionEngine 当前映射推导，外部只需传 CancelRequest。
    std::function<void(const utilities::CancelRequest&)> cancel_order;

    // 订单/成交查询与遍历
    std::function<utilities::OrderData*(const std::string&)> get_order;
    std::function<utilities::TradeData*(const std::string&)> get_trade;
    std::function<std::string(const std::string&)> get_strategy_name_for_order;
    std::function<std::vector<utilities::OrderData>()> get_all_orders;
    std::function<std::vector<utilities::TradeData>()> get_all_trades;
    std::function<std::vector<utilities::OrderData>()> get_all_active_orders;
    std::function<const std::unordered_map<std::string, std::set<std::string>>&()>
        get_strategy_active_orders;

    // 撤单/策略移除时，从 ExecutionEngine 中整理状态
    std::function<void(const std::string&)> remove_order_tracking;
    std::function<std::unordered_set<std::string>&()> get_active_order_ids;
    std::function<void(const std::string&)> ensure_strategy_key;
    std::function<void(const std::string&)> remove_strategy_tracking;
};

struct PortfolioAPI {
    // 组合 / 合约 / 策略持仓
    std::function<utilities::PortfolioData*(const std::string&)> get_portfolio;
    std::function<const utilities::ContractData*(const std::string&)> get_contract;
    std::function<utilities::StrategyHolding*(const std::string&)> get_holding;

    // 策略生命周期相关的 holding 管理
    std::function<void(const std::string& strategy_name)> get_or_create_holding;
    std::function<void(const std::string& strategy_name)> remove_strategy_holding;
};

struct SystemAPI {
    // 日志与策略事件
    std::function<void(const utilities::LogData&)> write_log;
    std::function<void(const utilities::StrategyUpdateData&)> put_strategy_event;

    // 辅助引擎（如对冲、组合构建）
    std::function<engines::ComboBuilderEngine*()> get_combo_builder_engine;
    std::function<engines::HedgeEngine*()> get_hedge_engine;
};

struct RuntimeAPI {
    ExecutionAPI execution;
    PortfolioAPI portfolio;
    SystemAPI system;
};

} // namespace core
