#pragma once

/**
 * The platform's names for instruments: one name per instrument, whatever the source or broker.
 * Each adapter translates its own identifiers (OCC codes, IB contracts, ...) to these and back;
 * nothing else builds a name or takes one apart.
 *
 *   underlying  SPY                   its ticker
 *   option      SPY-20261016-C-582.5  root-expiry-right-strike
 *   chain       SPY_20261016          root_expiry: a root's options of one expiry
 *
 * The backtest data names an option by its OCC code without the root (occ_code).
 *
 * An option's root (no '-' in it) is its trading class, SPXW for SPX's weeklies, and names the
 * portfolio the option is in.
 */

#include "model/constant.hpp"
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace model {

/** What names an option. */
struct OptionId {
    std::string root;
    std::chrono::year_month_day expiry{};
    OptionType type = OptionType::CALL;
    double strike = 0.0;

    bool operator==(const OptionId&) const = default;
};

std::string option_symbol(const OptionId& id);
/** The option a symbol names; nullopt for anything else (an underlying's ticker included). */
std::optional<OptionId> parse_option_symbol(std::string_view symbol);
std::string chain_symbol(std::string_view root, std::chrono::year_month_day expiry);
/** The option's OCC code without its root: YYMMDD, C or P, the strike in thousandths (8 digits):
 * 250219C00500000. */
std::string occ_code(const OptionId& id);
/** The option an OCC code (without its root) names on `root`; nullopt when it is not one. */
std::optional<OptionId> parse_occ_code(std::string_view root, std::string_view code);
/** A strike as option symbols and chain indexes write it: 580, 582.5. */
std::string strike_text(double strike);

} // namespace model
