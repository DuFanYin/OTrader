#pragma once

#include "../utilities/constant.hpp"
#include "../utilities/portfolio.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace core {
class OptionStrategyEngine;
}

namespace strategy_cpp {

class OptionStrategyTemplate {
  public:
    OptionStrategyTemplate(core::OptionStrategyEngine* strategy_engine, std::string strategy_name,
                           std::string portfolio_name,
                           const std::unordered_map<std::string, double>& setting);
    virtual ~OptionStrategyTemplate() = default;

    virtual void on_init_logic() = 0;
    virtual void on_stop_logic() = 0;
    virtual void on_timer_logic() = 0;
    virtual void on_order(const utilities::OrderData& order);
    virtual void on_trade(const utilities::TradeData& trade);

    void on_init();
    void on_start();
    void on_stop();
    void on_timer();

    void subscribe_chains(const std::vector<std::string>& chain_symbols);
    utilities::ChainData* get_chain(const std::string& chain_symbol) const;

    std::vector<std::string>
    underlying_order(utilities::Direction direction, double price, double volume = 1.0,
                     utilities::OrderType order_type = utilities::OrderType::MARKET);
    std::vector<std::string>
    option_order(const utilities::OptionData& option_data, utilities::Direction direction,
                 double price, double volume = 1.0,
                 utilities::OrderType order_type = utilities::OrderType::MARKET);
    std::vector<std::string>
    combo_order(utilities::ComboType combo_type,
                const std::unordered_map<std::string, utilities::OptionData*>& option_data,
                utilities::Direction direction, double price, double volume = 1.0,
                utilities::OrderType order_type = utilities::OrderType::MARKET);
    void register_hedging(int timer_trigger = 5, int delta_target = 0, int delta_range = 0);
    void unregister_hedging();

    void close_all_strategy_positions();
    void set_error(const std::string& msg = "");
    void write_log(const std::string& msg) const;

    const std::string& strategy_name() const { return strategy_name_; }
    const std::string& portfolio_name() const { return portfolio_name_; }
    bool inited() const { return inited_; }
    bool started() const { return started_; }
    bool error() const { return error_; }
    const std::string& error_msg() const { return error_msg_; }
    utilities::StrategyHolding* holding() const { return holding_; }
    void set_holding(utilities::StrategyHolding* h) { holding_ = h; }
    utilities::PortfolioData* portfolio() const { return portfolio_; }
    utilities::UnderlyingData* underlying() const { return underlying_; }

  protected:
    core::OptionStrategyEngine* engine_ = nullptr;
    std::string strategy_name_;
    std::string portfolio_name_;
    utilities::PortfolioData* portfolio_ = nullptr;
    utilities::UnderlyingData* underlying_ = nullptr;
    utilities::StrategyHolding* holding_ = nullptr;
    std::unordered_map<std::string, utilities::ChainData*> chain_map_;
    bool inited_ = false;
    bool started_ = false;
    bool error_ = false;
    std::string error_msg_;
    int timer_trigger_ = 1;
    int timer_cnt_ = 0;
};

} // namespace strategy_cpp
