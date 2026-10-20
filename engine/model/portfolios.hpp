#pragma once

/** Portfolios: the portfolios an engine trades, for backtest and live. */

#include "model/object.hpp"
#include "model/portfolio.hpp"
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace model {

/** The portfolios by name, and which one a contract is in: an option in its root's, an underlying
 * in the one it underlies or hedges (SPY is SPY's underlying and SPXW's hedge instrument). */
class Portfolios {
  public:
    Portfolios() = default;

    /** The portfolios of model::kPortfolios. */
    void ensure_portfolios_created();
    /** Of its spec in model::kPortfolios, else of none. */
    void ensure_portfolio(const std::string& portfolio_name);
    /** The spec must outlive it. */
    void ensure_portfolio(const PortfolioSpec& spec);
    void clear();
    /** Contracts added to the portfolios they are in: an option to its root's, an underlying to
     * the one it underlies (else the one named after it) and those hedged with it. A portfolio not
     * created yet takes none. */
    void load_contracts(std::span<const Contract> contracts);
    /** A trading day's contracts in place of the portfolio's (Portfolio::replace_options): an
     * option not in them goes unless `keep` names it. Returns the chains dropped. */
    std::vector<std::string> replace_contracts(const std::string& portfolio_name,
                                               std::span<const Contract> contracts,
                                               const std::unordered_set<std::string>& keep);
    /** The same, the day's options listed already, and its underlying; `valued`: the chains the
     * portfolio's frame holds values of, when known (Portfolio::list). */
    std::vector<std::string>
    replace_contracts(const std::string& portfolio_name, const Contract& underlying,
                      std::shared_ptr<const Listing> listing,
                      std::optional<std::span<const std::string>> valued = {});

    Portfolio* find_portfolio(const std::string& portfolio_name);
    const Portfolio* find_portfolio(const std::string& portfolio_name) const;
    std::vector<std::string> portfolio_names() const;
    /** One of a portfolio's options, its underlying or its hedge instrument. */
    [[nodiscard]] bool trades_contract(std::string_view symbol) const;
    /** An option's root's, else the one whose underlying or hedge instrument it is; null: none. */
    const Portfolio* portfolio_of(std::string_view symbol) const;
    /** Valid until the portfolio's next roll. */
    const Contract* find_contract(std::string_view symbol) const;
    /** Its contract's; one not listed (an option expired): Contract::kOptionMultiplierDefault for
     * an option, else 1. */
    [[nodiscard]] double multiplier(std::string_view symbol) const;
    /** The calendar of the contract's portfolio, else of its root's spec, else NYSE's. */
    [[nodiscard]] const SessionCalendar& calendar_of(std::string_view symbol) const;

  private:
    /** Into the portfolios it underlies or hedges, by their specs; else the one named after it. */
    void load_underlying(const Contract& contract);

    std::unordered_map<std::string, std::unique_ptr<Portfolio>> portfolios_;
};

} // namespace model
