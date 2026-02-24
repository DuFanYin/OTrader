#pragma once

/**
 * 统一策略引擎（core）：策略实例 + OMS 状态，能力通过 RuntimeAPI 由 runtime 注入。
 * backtest 与 live 共用本实现；不引入 interface，不使用 current_strategy_name_。
 */

#include "../utilities/constant.hpp"
#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"
#include "../utilities/event.hpp"
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engines {
class ComboBuilderEngine;
class HedgeEngine;
}

namespace strategy_cpp {
class OptionStrategyTemplate;
}

namespace core {

/** 由 runtime（MainEngine）实现并注入；get_portfolio/get_contract/get_holding 非策略引擎职责，仅转发。 */
struct RuntimeAPI {
    std::function<std::string(const std::string& strategy_name, const utilities::OrderRequest&)> send_order;
    std::function<std::string(const std::string& strategy_name, utilities::ComboType combo_type,
                              const std::string& combo_sig, utilities::Direction direction,
                              double price, double volume, const std::vector<utilities::Leg>& legs,
                              utilities::OrderType order_type)> send_combo_order;
    std::function<void(const utilities::LogData&)> write_log;
    std::function<utilities::PortfolioData*(const std::string&)> get_portfolio;
    std::function<const utilities::ContractData*(const std::string&)> get_contract;
    std::function<utilities::StrategyHolding*(const std::string&)> get_holding;
    std::function<void(const std::string& strategy_name)> get_or_create_holding;
    std::function<void(const std::string& strategy_name)> remove_strategy_holding;
    std::function<engines::ComboBuilderEngine*()> get_combo_builder_engine;
    std::function<engines::HedgeEngine*()> get_hedge_engine;
    std::function<void(const utilities::StrategyUpdateData&)> put_strategy_event;
};

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

    utilities::PortfolioData* get_portfolio(const std::string& portfolio_name);
    utilities::StrategyHolding* get_holding(const std::string& strategy_name);
    const utilities::ContractData* get_contract(const std::string& symbol) const;
    void write_log(const std::string& msg, int level = 0);
    void write_log(const utilities::LogData& log);

    /** 显式带 strategy_name，内部转 api_.send_order。 */
    std::string send_order(const std::string& strategy_name, const utilities::OrderRequest& req);
    /** 便捷：按 symbol 构造 OrderRequest 后调用 send_order(strategy_name, req)。 */
    std::vector<std::string> send_order(const std::string& strategy_name, const std::string& symbol,
                                        utilities::Direction direction, double price, double volume,
                                        utilities::OrderType order_type = utilities::OrderType::LIMIT);
    std::vector<std::string> send_combo_order(const std::string& strategy_name,
                                              utilities::ComboType combo_type,
                                              const std::string& combo_sig,
                                              utilities::Direction direction,
                                              double price, double volume,
                                              const std::vector<utilities::Leg>& legs,
                                              utilities::OrderType order_type = utilities::OrderType::LIMIT);

    void add_strategy(const std::string& class_name,
                      const std::string& portfolio_name,
                      const std::unordered_map<std::string, double>& setting = {});

    void init_strategy(const std::string& strategy_name);
    void start_strategy(const std::string& strategy_name);
    void stop_strategy(const std::string& strategy_name);
    /** 移除策略并清理 holding；返回是否成功移除。 */
    bool remove_strategy(const std::string& strategy_name);

    /** 遍历所有策略调用 on_timer（供 event 驱动）。 */
    void on_timer();
    utilities::OrderData* get_order(const std::string& orderid);
    utilities::TradeData* get_trade(const std::string& tradeid);
    /** 供 live 在 process_order 后根据 orderid 取 strategy_name 以 save_order_data。 */
    std::string get_strategy_name_for_order(const std::string& orderid) const;
    std::vector<utilities::OrderData> get_all_orders() const;
    std::vector<utilities::TradeData> get_all_trades() const;
    std::vector<utilities::OrderData> get_all_active_orders() const;
    const std::unordered_map<std::string, std::set<std::string>>& get_strategy_active_orders();
    /** 已加载的策略名列表（供 live event 遍历 hedge 等）。 */
    std::vector<std::string> get_strategy_names() const;

    void close();

    engines::ComboBuilderEngine* combo_builder_engine();
    engines::HedgeEngine* hedge_engine();

    /** 供 backtest MainEngine 在 append_order 后插入 orderid 使用。 */
    std::unordered_set<std::string>& active_order_ids() { return all_active_order_ids_; }
    /** 供 runtime 在 cancel 时从引擎侧移除 orderid 跟踪。 */
    void remove_order_tracking(const std::string& orderid);

private:
    RuntimeAPI api_;
    std::unordered_map<std::string, std::unique_ptr<strategy_cpp::OptionStrategyTemplate>> strategies_;
    std::unordered_map<std::string, utilities::OrderData> orders_;
    std::unordered_map<std::string, utilities::TradeData> trades_;
    std::unordered_map<std::string, std::set<std::string>> strategy_active_orders_;
    std::unordered_map<std::string, std::string> orderid_strategy_name_;
    std::unordered_set<std::string> all_active_order_ids_;
    mutable std::unordered_map<std::string, std::set<std::string>> hedge_active_orders_cache_;
};

}  // namespace core
