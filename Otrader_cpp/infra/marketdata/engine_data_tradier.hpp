#pragma once

/**
 * MarketDataEngine (live): portfolios + contracts; portfolio 结构仅由 Contract 事件建立。
 * 行情/ Greeks 更新由 Snapshot 事件经 EventEngine → get_portfolio()->apply_frame(snapshot)。
 * - process_contract: 处理 Contract 事件，维护
 * contracts_、portfolios_（add_option/set_underlying）， 每次 add_option 后 finalize_chains 以便
 * apply_frame 可用。
 * - get_portfolio / get_contract: 供 MainEngine、EventEngine、Strategy 等使用。
 * 接入真实行情时：组装 PortfolioSnapshot 并 put_event(EventType::Snapshot, snapshot) 即可。
 */

#include "../../core/engine_log.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../utilities/event.hpp"
#include "../../utilities/object.hpp"
#include "../../utilities/portfolio.hpp"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace engines {

struct MainEngine;

class MarketDataEngine : public utilities::BaseEngine {
  public:
    explicit MarketDataEngine(utilities::MainEngine* main_engine);

    void process_contract(const utilities::Event& event);

    void subscribe_chains(const std::string& strategy_name,
                          const std::vector<std::string>& chain_symbols);
    void unsubscribe_chains(const std::string& strategy_name);

    utilities::PortfolioData* get_or_create_portfolio(const std::string& portfolio_name);
    utilities::PortfolioData* get_portfolio(const std::string& portfolio_name);
    std::vector<std::string> get_all_portfolio_names() const;
    const utilities::ContractData* get_contract(const std::string& symbol) const;
    std::vector<utilities::ContractData> get_all_contracts() const;

    void start_market_data_update();
    void stop_market_data_update();

  private:
    void write_log(const std::string& msg, int level = INFO);

    std::unordered_map<std::string, std::unique_ptr<utilities::PortfolioData>> portfolios_;
    std::unordered_map<std::string, utilities::ContractData> contracts_;
    std::unordered_map<std::string, std::set<std::string>> active_chains_;
    std::unordered_map<std::string, std::set<std::string>> strategy_chains_;
    bool started_ = false;
};

} // namespace engines
