#pragma once

/** The portfolios the engine knows. Each is named after its options' trading class (SPXW for
 * SPX's weeklies), is on one underlying, and is delta-hedged with one instrument: the underlying
 * itself when it trades, else one that moves with it (SPX and XSP, indexes, do not trade; SPY
 * does, at a tenth of SPX, about XSP). Its options settle at expiry in shares (SPY's, QQQ's,
 * IWM's) or in cash (SPXW's, XSP's): BookkeepingEngine::settle_expiry. Its exchange's fee an
 * option contract, beyond the broker's commission: SPX's options pay Cboe's index fee; on the
 * ETFs' the broker's commission holds the exchanges' (IBKR's fixed rate). */

#include "model/constant.hpp"
#include "model/session_calendar.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace model {

/** A fee by an option contract's price: the first tier whose bound the price is below (the last
 * tier's bound is none), USD a contract. */
struct FeeTier {
    double below = std::numeric_limits<double>::infinity();
    double fee = 0.0;
};

/** The fee of the first tier `price` is below; 0 past the last or with none. */
constexpr auto tier_fee(std::span<const FeeTier> tiers, double price) -> double {
    for (const FeeTier& t : tiers) {
        if (price < t.below) {
            return t.fee;
        }
    }
    return 0.0;
}

inline constexpr std::array kCboeIndexFee{FeeTier{.below = 1.0, .fee = 0.50}, FeeTier{.fee = 0.59}};

/** What a portfolio is: the one place it is said (model::Portfolio reads it). */
struct PortfolioSpec {
    std::string_view name;
    std::string_view underlying; // its symbol, the first field of its contract's symbol
    Product underlying_product = Product::EQUITY; // EQUITY trades, INDEX does not
    std::string_view hedge;      // the symbol a strategy's delta hedge trades for it
    bool physical = false;       // its options settle in shares of the underlying (else in cash)
    const SessionCalendar* calendar = &kNyse; // its days and hours, its options' expiry
    std::span<const FeeTier> exchange_fee;    // its options' a contract, by price; none: 0
};

inline constexpr std::array kPortfolios{
    PortfolioSpec{.name = "SPY", .underlying = "SPY", .underlying_product = Product::EQUITY,
                  .hedge = "SPY", .physical = true},
    PortfolioSpec{.name = "SPXW", .underlying = "SPX", .underlying_product = Product::INDEX,
                  .hedge = "SPY", .physical = false, .exchange_fee = kCboeIndexFee},
    PortfolioSpec{.name = "QQQ", .underlying = "QQQ", .underlying_product = Product::EQUITY,
                  .hedge = "QQQ", .physical = true},
    PortfolioSpec{.name = "IWM", .underlying = "IWM", .underlying_product = Product::EQUITY,
                  .hedge = "IWM", .physical = true},
    PortfolioSpec{.name = "XSP", .underlying = "XSP", .underlying_product = Product::INDEX,
                  .hedge = "SPY", .physical = false},
};

/** The portfolio named `name`, or nullptr. */
constexpr auto find_portfolio(std::string_view name) -> const PortfolioSpec* {
    const auto it = std::ranges::find(kPortfolios, name, &PortfolioSpec::name);
    return it != kPortfolios.end() ? &*it : nullptr;
}

/** The calendar of the portfolio named `name` (an option's root, a contract's trading class):
 * its spec's, else NYSE's. */
inline auto calendar_of(std::string_view name) -> const SessionCalendar& {
    const PortfolioSpec* spec = find_portfolio(name);
    return spec != nullptr ? *spec->calendar : kNyse;
}

/** The underlying a portfolio's options are on (SPXW -> SPX): the declared one, else the
 * portfolio's name. */
inline auto underlying_of(std::string_view portfolio) -> std::string {
    const PortfolioSpec* spec = find_portfolio(portfolio);
    return std::string(spec != nullptr ? spec->underlying : portfolio);
}

} // namespace model
