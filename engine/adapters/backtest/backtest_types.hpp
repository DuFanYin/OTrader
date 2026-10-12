#pragma once

/** What the backtest's data loader reads a day into. */

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace adapters {

using Timestamp = std::chrono::system_clock::time_point;

struct DataMeta {
    std::string path;
    int64_t row_count = 0;
    std::string time_column;
    std::string ts_start;
    std::string ts_end;
};

} // namespace adapters
