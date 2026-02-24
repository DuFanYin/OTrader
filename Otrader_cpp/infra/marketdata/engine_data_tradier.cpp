/**
 * MarketDataEngine (live): portfolio 由 Contract 事件建立，行情由 Snapshot 事件更新。
 * process_contract 维护 contracts_ 与 portfolios_，finalize_chains 保证 apply_frame 可用。
 */

#include "engine_data_tradier.hpp"
#include "../../utilities/event.hpp"
#include <chrono>
#include <variant>
#include <iomanip>
#include <sstream>

namespace engines {

MarketDataEngine::MarketDataEngine(utilities::MainEngine* main_engine)
    : BaseEngine(main_engine, "MarketData") {}

void MarketDataEngine::write_log(const std::string& msg, int level) {
    if (main_engine) main_engine->write_log(msg, level, engine_name);
}

void MarketDataEngine::process_contract(const utilities::Event& event) {
    if (const auto* p = std::get_if<utilities::ContractData>(&event.data)) {
        contracts_[p->symbol] = *p;
        size_t pos = p->symbol.find('-');
        std::string underlying_name = (pos != std::string::npos) ? p->symbol.substr(0, pos) : p->symbol;
        utilities::PortfolioData* port = get_or_create_portfolio(underlying_name);
        if (p->product == utilities::Product::OPTION) {
            port->add_option(*p);
            port->finalize_chains();  // keep option_apply_order_ ready for Snapshot apply_frame
        } else if (p->product == utilities::Product::EQUITY || p->product == utilities::Product::INDEX) {
            port->set_underlying(*p);
        }
    }
}

void MarketDataEngine::subscribe_chains(const std::string& strategy_name, const std::vector<std::string>& chain_symbols) {
    for (const auto& chain_symbol : chain_symbols) {
        strategy_chains_[strategy_name].insert(chain_symbol);
        size_t i = chain_symbol.find('_');
        std::string portfolio_name = (i != std::string::npos) ? chain_symbol.substr(0, i) : chain_symbol;
        active_chains_[portfolio_name].insert(chain_symbol);
    }
    write_log("Strategy " + strategy_name + " subscribed to chains", INFO);
}

void MarketDataEngine::unsubscribe_chains(const std::string& strategy_name) {
    auto it = strategy_chains_.find(strategy_name);
    if (it == strategy_chains_.end()) return;
    for (const auto& chain_symbol : it->second) {
        size_t i = chain_symbol.find('_');
        std::string portfolio_name = (i != std::string::npos) ? chain_symbol.substr(0, i) : chain_symbol;
        active_chains_[portfolio_name].erase(chain_symbol);
        if (active_chains_[portfolio_name].empty()) active_chains_.erase(portfolio_name);
    }
    strategy_chains_.erase(it);
    write_log("Strategy " + strategy_name + " unsubscribed from all chains", INFO);
}

utilities::PortfolioData* MarketDataEngine::get_or_create_portfolio(const std::string& portfolio_name) {
    auto it = portfolios_.find(portfolio_name);
    if (it != portfolios_.end()) return it->second.get();
    portfolios_[portfolio_name] = std::make_unique<utilities::PortfolioData>(portfolio_name);
    return portfolios_[portfolio_name].get();
}

utilities::PortfolioData* MarketDataEngine::get_portfolio(const std::string& portfolio_name) {
    auto it = portfolios_.find(portfolio_name);
    return (it != portfolios_.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> MarketDataEngine::get_all_portfolio_names() const {
    std::vector<std::string> out;
    for (const auto& kv : portfolios_) out.push_back(kv.first);
    return out;
}

const utilities::ContractData* MarketDataEngine::get_contract(const std::string& symbol) const {
    auto it = contracts_.find(symbol);
    return (it != contracts_.end()) ? &it->second : nullptr;
}

std::vector<utilities::ContractData> MarketDataEngine::get_all_contracts() const {
    std::vector<utilities::ContractData> out;
    for (const auto& kv : contracts_) out.push_back(kv.second);
    return out;
}

void MarketDataEngine::start_market_data_update() {
    started_ = true;
    write_log("Market data update started (stub)", INFO);
}

void MarketDataEngine::stop_market_data_update() {
    started_ = false;
}

}  // namespace engines
