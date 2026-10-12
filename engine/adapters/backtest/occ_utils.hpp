#pragma once

/** The underlying a backtest day is of, from its file's name. */

#include <string>

namespace adapters {

/** backtest_<UNDERLYING>_<start>_<end>.parquet, or data/<UNDERLYING>/<UNDERLYING>-YYYY-MM/<day>. */
std::string infer_underlying_from_filename(const std::string& filename);

} // namespace adapters
