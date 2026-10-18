#pragma once

/** The portfolios the engine knows. Each is named after its options' trading class (SPXW for
 * SPX's weeklies), is on one underlying, and is delta-hedged with one instrument: the underlying
 * itself when it trades, else one that moves with it (SPX and XSP, indexes, do not trade; SPY
 * does, at a tenth of SPX, about XSP). Its options settle at expiry in shares (SPY's, QQQ's,
 * IWM's) or in cash (SPXW's, XSP's): BookkeepingEngine::settle_expiry. */

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace model {

struct PortfolioSpec {
    std::string_view name;
    std::string_view underlying; // its symbol, the first field of its contract's symbol
    std::string_view hedge;      // the symbol a strategy's delta hedge trades for it
    bool physical = false;       // its options settle in shares of the underlying (else in cash)
};

inline constexpr std::array kPortfolios{
    PortfolioSpec{.name = "SPY", .underlying = "SPY", .hedge = "SPY", .physical = true},
    PortfolioSpec{.name = "SPXW", .underlying = "SPX", .hedge = "SPY", .physical = false},
    PortfolioSpec{.name = "QQQ", .underlying = "QQQ", .hedge = "QQQ", .physical = true},
    PortfolioSpec{.name = "IWM", .underlying = "IWM", .hedge = "IWM", .physical = true},
    PortfolioSpec{.name = "XSP", .underlying = "XSP", .hedge = "SPY", .physical = false},
};

/** The portfolio named `name`, or nullptr. */
constexpr auto find_portfolio(std::string_view name) -> const PortfolioSpec* {
    const auto it = std::ranges::find(kPortfolios, name, &PortfolioSpec::name);
    return it != kPortfolios.end() ? &*it : nullptr;
}

/** The underlying a portfolio's options are on (SPXW -> SPX): the declared one, else the
 * portfolio's name. */
inline auto underlying_of(std::string_view portfolio) -> std::string {
    const PortfolioSpec* spec = find_portfolio(portfolio);
    return std::string(spec != nullptr ? spec->underlying : portfolio);
}

} // namespace model
