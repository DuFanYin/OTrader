#pragma once

/**
 * OCC option symbol parsing (matches Python engine_data._parse_occ_symbol).
 * Format e.g. "250219C00500000" -> expiry, strike, option_type.
 */

#include "constant.hpp"
#include <chrono>
#include <optional>
#include <string>
#include <tuple>

namespace backtest {

using Timestamp = std::chrono::system_clock::time_point;

/** Returns (expiry, strike, option_type) or (nullopt, nullopt, nullopt) if invalid. */
std::tuple<std::optional<Timestamp>, std::optional<double>, std::optional<utilities::OptionType>>
parse_occ_symbol(const std::string& symbol);

/** Infer underlying from filename: backtest_<UNDERLYING>_<start>_<end>.parquet */
std::string infer_underlying_from_filename(const std::string& filename);

/** Format expiry as YYYYMMDD string. */
std::string format_expiry_yyyymmdd(Timestamp expiry);

} // namespace backtest
