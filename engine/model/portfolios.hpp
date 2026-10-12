#pragma once

/** The portfolios the live engine trades. Each is named after its options' trading class (SPXW
 * for SPX's weeklies), is on one underlying, and is delta-hedged with one instrument: the
 * underlying itself when it trades, else one that moves with it (SPX, an index, does not trade;
 * SPY does, at a tenth of it). Its options settle at expiry in shares (SPY's) or in cash (SPXW's):
 * PositionEngine::settle_expiry. */

#include <algorithm>
#include <array>
#include <string_view>

namespace model {

struct PortfolioSpec {
    std::string_view name;
    std::string_view underlying; // its symbol, the first field of its contract's symbol
    std::string_view hedge;      // the symbol the HedgeEngine trades for it
    bool physical = false;       // its options settle in shares of the underlying (else in cash)
};

inline constexpr std::array kPortfolios{
    PortfolioSpec{.name = "SPY", .underlying = "SPY", .hedge = "SPY", .physical = true},
    PortfolioSpec{.name = "SPXW", .underlying = "SPX", .hedge = "SPY", .physical = false},
};

/** The portfolio named `name`, or nullptr. */
constexpr auto find_portfolio(std::string_view name) -> const PortfolioSpec* {
    const auto it = std::ranges::find(kPortfolios, name, &PortfolioSpec::name);
    return it != kPortfolios.end() ? &*it : nullptr;
}

} // namespace model
