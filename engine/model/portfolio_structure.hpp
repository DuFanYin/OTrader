#pragma once

/**
 * PortfolioStructure: portfolios + contracts from load_contracts.
 * Shared by backtest and live. Used by Runtime (structure only) and MarketDataEngine (inherits).
 */

#include "object.hpp"
#include "portfolio.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace model {

class PortfolioStructure {
  public:
    PortfolioStructure() = default;

    void ensure_portfolios_created();
    /** Create portfolio by name if not exists (for backtest). */
    void ensure_portfolio(const std::string& portfolio_name);
    /** Clear all portfolios and contracts (for backtest reload). */
    void clear();
    void process_option(const model::ContractData& contract);
    void process_underlying(const model::ContractData& contract);
    /** Feed contract into a specific portfolio (for backtest). */
    void process_option_for_portfolio(const std::string& portfolio_name,
                                      const model::ContractData& contract);
    void process_underlying_for_portfolio(const std::string& portfolio_name,
                                          const model::ContractData& contract);
    void finalize_all_chains();

    model::PortfolioData* get_portfolio(const std::string& portfolio_name);
    const model::PortfolioData* get_portfolio(const std::string& portfolio_name) const;
    std::vector<std::string> get_all_portfolio_names() const;
    /** A contract one of the portfolios trades: one of its options, its underlying or its hedge
     * instrument. */
    [[nodiscard]] bool trades_contract(std::string_view symbol) const;
    /** The portfolio a contract is in: an option's (its root), the one whose underlying or hedge
     * instrument it is; null for none. */
    const model::PortfolioData* portfolio_of(std::string_view symbol) const;
    const model::ContractData* get_contract(const std::string& symbol) const;
    std::vector<model::ContractData> get_all_contracts() const;

  protected:
    model::PortfolioData* get_or_create_portfolio(const std::string& portfolio_name);
    void process_contract(const model::ContractData& contract, bool is_option);

    std::unordered_map<std::string, std::unique_ptr<model::PortfolioData>> portfolios_;
    std::unordered_map<std::string, model::ContractData> contracts_;
};

} // namespace model
