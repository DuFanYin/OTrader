#pragma once

/**
 * IB contracts in the platform's terms (utilities/symbol.hpp). IB's identifiers are translated
 * here, both ways, and nowhere else:
 *   an underlying  SPY STK, SPX IND              <->  SPY, SPX (its ticker)
 *   an option      SPX OPT SPXW 20261016 C 5800  <->  SPXW-20261016-C-5800 (root: trading class)
 */

#include "../../model/object.hpp"

#include "Contract.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adapters {

/** How IB lists an underlying the engine trades (core/portfolios.hpp): the contract sync asks IB
 * for it and its option chains. */
struct IbListing {
    std::string_view symbol;
    std::string_view sec_type; // STK or IND
    std::string_view exchange; // SMART, or the exchange an index is listed on
};

inline constexpr std::array kIbListings{
    IbListing{"SPY", "STK", "SMART"},
    IbListing{"SPX", "IND", "CBOE"},
};

/** How IB lists `symbol`, or nullptr. */
constexpr auto find_listing(std::string_view symbol) -> const IbListing* {
    const auto it = std::ranges::find(kIbListings, symbol, &IbListing::symbol);
    return it != kIbListings.end() ? &*it : nullptr;
}

/** The platform's symbol for an IB contract. */
std::string platform_symbol(const Contract& contract);

/** The IB contract a platform symbol names: an option, or an underlying as IB lists it
 * (kIbListings; else a stock on SMART). */
Contract ib_contract(const std::string& symbol);

/** The IB contract for a combo: a BAG of its legs, by contract id. */
Contract ib_combo_contract(std::span<const model::Leg> legs, const std::string& trading_class);

/** The platform's contract for the underlying IB describes. */
model::ContractData underlying_contract(const ContractDetails& details);

/** The platform's contract for the option IB describes, in the portfolio its trading class names;
 * name is its OCC code (IB's local symbol). */
model::ContractData option_contract(const ContractDetails& details,
                                        const std::string& underlying_symbol);

/** The expirations (IB's YYYYMMDD) from `from` to max_dte days later, in order. */
std::vector<std::string> expiries_within(const std::set<std::string>& expirations,
                                         std::chrono::year_month_day from, int max_dte);

} // namespace adapters
