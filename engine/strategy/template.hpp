#pragma once

#include "../model/combo_builder.hpp"
#include "../model/constant.hpp"
#include "../utilities/log_level.hpp"
#include "../model/portfolio.hpp"
#include "../model/utility.hpp"
#include "strategy_context.hpp"
#include "strategy_param.hpp"
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace strategy {

/**
 * Base of every strategy. A strategy reads the market and its own holding (read-only views of
 * engine state) and acts through the order helpers below, all through its StrategyContext. Declare
 * its parameters with a static parameters() and register it with REGISTER_STRATEGY in its .cpp.
 */
class OptionStrategyTemplate {
  public:
    OptionStrategyTemplate(StrategyContext* context, std::string strategy_name,
                           std::string portfolio_name,
                           const std::unordered_map<std::string, double>& setting);
    virtual ~OptionStrategyTemplate() = default;

    /** Parameters every strategy has (the template reads them). */
    static std::span<const StrategyParam> base_parameters();
    /** A strategy's own parameters; a strategy that has some declares its own parameters(). */
    static std::span<const StrategyParam> parameters() { return {}; }

    virtual void on_init_logic() = 0;
    virtual void on_stop_logic() = 0;
    /** Once per bar (a minute of event time; every timer_trigger bars), in backtest and live. */
    virtual void on_timer_logic() = 0;
    /** A snapshot of the portfolio was applied: its quotes, forwards, IV and Greeks are fresh.
     * Live on each batch of quotes that changes them; a backtest once a bar, before the bar's
     * timer. While started. For what must answer the market at once; the default does nothing. */
    virtual void on_snapshot() {}
    virtual void on_order(const model::OrderData& order);
    virtual void on_trade(const model::TradeData& trade);

    void on_init();
    void on_start();
    void on_stop();
    void on_timer();

    /** Event time: when the portfolio's last quotes were taken (backtest: the bar's time). Use it,
     * not timer counts, for anything about the clock. */
    model::DateTime now() const { return portfolio_->datetime(); }

    /** The chains the strategy trades: get_chain finds them, and live streams their quotes. */
    void subscribe_chains(std::span<const std::string> chain_symbols);
    const model::ChainData* get_chain(const std::string& chain_symbol) const;

    std::vector<std::string>
    underlying_order(model::Direction direction, double price, double volume = 1.0,
                     model::OrderType order_type = model::OrderType::MARKET);
    /** Send a combo built from option_data by combo_type; SINGLE_LEG sends the option's own
     * order. */
    std::vector<std::string>
    option_order(model::ComboType combo_type,
                 const std::unordered_map<std::string, const model::OptionData*>& option_data,
                 model::Direction direction, double price, double volume = 1.0,
                 model::OrderType order_type = model::OrderType::MARKET);
    /** A leg for custom_combo_order: the option, its side, and its contracts per combo unit. */
    model::Leg combo_leg(const model::OptionData& option, model::Direction direction,
                         int ratio) const;
    /** Send a CUSTOM combo of these legs, each on its own side and ratio; a single leg at ratio 1
     * is that option's own order. */
    std::vector<std::string> custom_combo_order(std::span<const model::Leg> legs, double price,
                                                double volume = 1.0,
                                                model::OrderType order_type =
                                                    model::OrderType::MARKET);
    void register_hedging(int timer_trigger = 5, int delta_target = 0, int delta_range = 0);
    void unregister_hedging();

    void close_all_strategy_positions();
    /** Close an option position at market: an option with its own order, a combo with one order
     * over the non-zero legs of `pos` (pass a subset of a combo's legs to close only those). */
    void close_strategy_position(const model::OptionPositionData& pos);
    /** True while an order this strategy sent is still working. */
    bool has_active_order() const;
    void set_error(const std::string& msg = "");
    void write_log(const std::string& msg, utilities::LogLevel level = utilities::INFO) const;

    const std::string& strategy_name() const { return strategy_name_; }
    /** The class it was created as (StrategyRegistry::create). */
    const std::string& class_name() const { return class_name_; }
    void set_class_name(const std::string& class_name) { class_name_ = class_name; }
    const std::string& portfolio_name() const { return portfolio_name_; }
    /** The resolved setting: every declared parameter's value. */
    const std::unordered_map<std::string, double>& setting() const { return setting_; }
    bool inited() const { return inited_; }
    bool started() const { return started_; }
    bool error() const { return error_; }
    const std::string& error_msg() const { return error_msg_; }
    const model::StrategyHolding* holding() const { return holding_; }
    void set_holding(const model::StrategyHolding* h) { holding_ = h; }
    const model::PortfolioData* portfolio() const { return portfolio_; }
    const model::UnderlyingData* underlying() const { return underlying_; }

  protected:
    /** A declared parameter's value from the resolved setting; param<int> for whole numbers. */
    template <typename T = double> T param(const std::string& name) const {
        return static_cast<T>(param_value(name));
    }

  private:
    double param_value(const std::string& name) const;
    /** Combo legs take their contracts (ids, trading class) from the context. */
    model::combo::ComboBuildOptions combo_options() const;

    StrategyContext* context_ = nullptr;
    std::string strategy_name_;
    std::string class_name_;
    std::string portfolio_name_;
    std::unordered_map<std::string, double> setting_;
    const model::PortfolioData* portfolio_ = nullptr;
    const model::UnderlyingData* underlying_ = nullptr;
    const model::StrategyHolding* holding_ = nullptr;
    std::unordered_map<std::string, const model::ChainData*> chain_map_;
    bool inited_ = false;
    bool started_ = false;
    bool error_ = false;
    std::string error_msg_;
    int timer_trigger_ = 1;
    int timer_cnt_ = 0;
    model::BarGate bar_gate_;
};

} // namespace strategy
