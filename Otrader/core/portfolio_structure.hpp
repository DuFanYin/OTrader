#pragma once

/**
 * PortfolioStructure: portfolios + contracts from load_contracts.
 * Shared by backtest and live. Used by Runtime (structure only) and MarketDataEngine (inherits).
 */

#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engines {

class PortfolioStructure {
  public:
    PortfolioStructure() = default;

    void ensure_portfolios_created();
    /** Create portfolio by name if not exists (for backtest). */
    void ensure_portfolio(const std::string& portfolio_name);
    /** Clear all portfolios and contracts (for backtest reload). */
    void clear();
    void process_option(const utilities::ContractData& contract);
    void process_underlying(const utilities::ContractData& contract);
    /** Feed contract into a specific portfolio (for backtest). */
    void process_option_for_portfolio(const std::string& portfolio_name,
                                      const utilities::ContractData& contract);
    void process_underlying_for_portfolio(const std::string& portfolio_name,
                                          const utilities::ContractData& contract);
    void finalize_all_chains();

    utilities::PortfolioData* get_portfolio(const std::string& portfolio_name);
    std::vector<std::string> get_all_portfolio_names() const;
    const utilities::ContractData* get_contract(const std::string& symbol) const;
    std::vector<utilities::ContractData> get_all_contracts() const;

  protected:
    utilities::PortfolioData* get_or_create_portfolio(const std::string& portfolio_name);
    void process_contract(const utilities::ContractData& contract, bool is_option);

    std::unordered_map<std::string, std::unique_ptr<utilities::PortfolioData>> portfolios_;
    std::unordered_map<std::string, utilities::ContractData> contracts_;
};

} // namespace engines
