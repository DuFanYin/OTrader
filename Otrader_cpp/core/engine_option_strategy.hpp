#pragma once

/**
 * 统一策略引擎（core）：策略实例 + OMS 状态，能力通过 RuntimeAPI 由 runtime 注入。
 * backtest 与 live 共用本实现；不引入 interface，不使用 current_strategy_name_。
 *
 * RuntimeAPI 的具体结构定义在 runtime_api.hpp 中（Execution/Portfolio/System 三块）。
 */

#include "../utilities/constant.hpp"
#include "../utilities/event.hpp"
#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"
#include "runtime_api.hpp"
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace strategy_cpp {
class OptionStrategyTemplate;
}

namespace core {

class OptionStrategyEngine {
  public:
    explicit OptionStrategyEngine(RuntimeAPI api);
    ~OptionStrategyEngine();

    void process_order(utilities::OrderData& order);
    void process_trade(const utilities::TradeData& trade);

    strategy_cpp::OptionStrategyTemplate* get_strategy(const std::string& strategy_name);
    /** 当仅有一个策略时（如 backtest）可返回该策略，否则返回 nullptr。 */
    strategy_cpp::OptionStrategyTemplate* get_strategy();
    utilities::StrategyHolding* get_strategy_holding(const std::string& strategy_name) const;
    /** 当仅有一个策略时返回其 holding。 */
    utilities::StrategyHolding* get_strategy_holding() const;

    utilities::PortfolioData* get_portfolio(const std::string& portfolio_name) const;
    utilities::StrategyHolding* get_holding(const std::string& strategy_name) const;
    const utilities::ContractData* get_contract(const std::string& symbol) const;
    void write_log(const std::string& msg, int level = 0) const;
    void write_log(const utilities::LogData& log) const;

    /** 显式带 strategy_name，内部转 api_.send_order。 */
    std::string send_order(const std::string& strategy_name,
                           const utilities::OrderRequest& req) const;
    /** 便捷：按 symbol 构造 OrderRequest 后调用 send_order(strategy_name, req)。 */
    std::vector<std::string>
    send_order(const std::string& strategy_name, const std::string& symbol,
               utilities::Direction direction, double price, double volume,
               utilities::OrderType order_type = utilities::OrderType::LIMIT);
    std::vector<std::string>
    send_combo_order(const std::string& strategy_name, utilities::ComboType combo_type,
                     const std::string& combo_sig, utilities::Direction direction, double price,
                     double volume, const std::vector<utilities::Leg>& legs,
                     utilities::OrderType order_type = utilities::OrderType::LIMIT);

    void add_strategy(const std::string& class_name, const std::string& portfolio_name,
                      const std::unordered_map<std::string, double>& setting = {});

    void init_strategy(const std::string& strategy_name);
    void start_strategy(const std::string& strategy_name);
    void stop_strategy(const std::string& strategy_name);
    /** 移除策略并清理 holding；返回是否成功移除。 */
    bool remove_strategy(const std::string& strategy_name);

    /** 遍历所有策略调用 on_timer（供 event 驱动）。 */
    void on_timer();
    utilities::OrderData* get_order(const std::string& orderid) const;
    utilities::TradeData* get_trade(const std::string& tradeid) const;
    /** 供 live 在 process_order 后根据 orderid 取 strategy_name 以 save_order_data。 */
    std::string get_strategy_name_for_order(const std::string& orderid) const;
    std::vector<utilities::OrderData> get_all_orders() const;
    std::vector<utilities::TradeData> get_all_trades() const;
    std::vector<utilities::OrderData> get_all_active_orders() const;
    const std::unordered_map<std::string, std::set<std::string>>&
    get_strategy_active_orders() const;
    /** 已加载的策略名列表（供 live event 遍历 hedge 等）。 */
    std::vector<std::string> get_strategy_names() const;

    void close();

    engines::ComboBuilderEngine* combo_builder_engine() const;
    engines::HedgeEngine* hedge_engine() const;

    /** 供 backtest MainEngine 在 append_order 后插入 orderid 使用。 */
    std::unordered_set<std::string>& active_order_ids() {
        return api_.execution.get_active_order_ids ? api_.execution.get_active_order_ids()
                                                   : dummy_active_order_ids_;
    }
    /** 供 runtime 在 cancel 时从引擎侧移除 orderid 跟踪。 */
    void remove_order_tracking(const std::string& orderid) const;

  private:
    RuntimeAPI api_;
    std::unordered_map<std::string, std::unique_ptr<strategy_cpp::OptionStrategyTemplate>>
        strategies_;
    std::unordered_set<std::string> dummy_active_order_ids_;

    // 统一的订单装配函数（单腿与组合均通过此处组装 OrderRequest）。
    bool assemble_order_request(const std::string& strategy_name, const std::string& symbol,
                                utilities::Direction direction, double price, double volume,
                                utilities::OrderType order_type,
                                const std::vector<utilities::Leg>* legs,
                                std::optional<utilities::ComboType> combo_type,
                                const std::string* combo_sig,
                                utilities::OrderRequest& out_req) const;
};

} // namespace core
