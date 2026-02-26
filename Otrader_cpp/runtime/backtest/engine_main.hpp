#pragma once

/**
 * MainEngine = 持有各引擎 + 提供 accessor + 执行（send_order/cancel_order/put_log_intent）。
 * 不包含 dispatch 控制逻辑；EventEngine 负责 dispatch，通过 main 的 accessor 访问各引擎。
 */

#include "../../core/engine_combo_builder.hpp"
#include "../../core/engine_hedge.hpp"
#include "../../core/engine_log.hpp"
#include "../../core/engine_option_strategy.hpp"
#include "../../core/engine_position.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/constant.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/portfolio.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace backtest {

class BacktestDataEngine;

class MainEngine : public utilities::MainEngine {
  public:
    static constexpr int INFO = 20;

    explicit MainEngine(utilities::IEventEngine* event_engine = nullptr);
    ~MainEngine();

    void register_portfolio(utilities::PortfolioData* portfolio);
    utilities::PortfolioData* get_portfolio(const std::string& portfolio_name) const;

    void register_contract(utilities::ContractData contract);
    const utilities::ContractData* get_contract(const std::string& symbol) const;

    BacktestDataEngine* load_backtest_data(const std::string& parquet_path,
                                           const std::string& underlying_symbol = "");
    BacktestDataEngine* get_data_engine() const { return data_engine_.get(); }

    std::string send_order(const utilities::OrderRequest& req);
    void add_order(std::string orderid, utilities::OrderData order);
    void cancel_order(const utilities::CancelRequest& req);

    utilities::OrderData* get_order(const std::string& orderid) const;
    utilities::TradeData* get_trade(const std::string& tradeid) const;
    std::vector<utilities::OrderData> get_all_orders() const;
    std::vector<utilities::TradeData> get_all_trades() const;
    std::vector<utilities::OrderData> get_all_active_orders() const;

    void put_event(const utilities::Event& e) override;

    /** Emit LogIntent; Runtime sinks it (no event bus). */
    void put_log_intent(const std::string& msg, int level = INFO) const;
    /** Emit LogIntent from payload (e.g. Strategy with gateway_name "Strategy"). */
    void put_log_intent(const utilities::LogData& intent) const;
    void close();

    /** Strategy/Hedge 只调 append_*，不持 event_engine；内部直接转
     * send_order/cancel_order/put_log_intent。 */
    std::string append_order(const utilities::OrderRequest& req);
    void append_cancel(const utilities::CancelRequest& req);
    void append_log(const utilities::LogData& log) const;

    /** Log level: only messages with level >= this are output. Use engines::DISABLED to turn off.
     */
    void set_log_level(int level);
    int log_level() const;

    utilities::IEventEngine* event_engine() { return event_engine_; }
    const utilities::IEventEngine* event_engine() const { return event_engine_; }

    core::OptionStrategyEngine* option_strategy_engine() { return option_strategy_engine_.get(); }
    const core::OptionStrategyEngine* option_strategy_engine() const {
        return option_strategy_engine_.get();
    }

    engines::PositionEngine* position_engine() { return position_engine_.get(); }
    const engines::PositionEngine* position_engine() const { return position_engine_.get(); }
    engines::ComboBuilderEngine* combo_builder_engine();
    engines::HedgeEngine* hedge_engine();
    utilities::StrategyHolding* get_holding(const std::string& strategy_name);
    const utilities::StrategyHolding* get_holding(const std::string& strategy_name) const;
    void get_or_create_holding(const std::string& strategy_name);

    using OrderExecutor = std::function<std::string(const utilities::OrderRequest&)>;
    void set_order_executor(OrderExecutor fn) { order_executor_ = std::move(fn); }

  private:
    utilities::IEventEngine* event_engine_ = nullptr;
    std::unordered_map<std::string, utilities::PortfolioData*> portfolios_;
    std::unordered_map<std::string, utilities::ContractData> contracts_;
    std::unordered_map<std::string, utilities::OrderData> orders_;
    int order_counter_ = 0;
    OrderExecutor order_executor_;
    std::unique_ptr<core::OptionStrategyEngine> option_strategy_engine_;
    std::unique_ptr<BacktestDataEngine> data_engine_;
    std::unique_ptr<engines::PositionEngine> position_engine_;
    std::unique_ptr<engines::ComboBuilderEngine> combo_builder_engine_;
    std::unique_ptr<engines::HedgeEngine> hedge_engine_;
    std::unique_ptr<engines::LogEngine> log_engine_;
};

} // namespace backtest
