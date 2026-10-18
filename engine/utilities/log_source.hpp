#pragma once

/** LogSource: what every engine holds as a member, a name and the log it writes to under that
 * name. Held, not inherited: an engine is never used as a LogSource. */

#include "utilities/log_level.hpp"
#include "utilities/logger.hpp"
#include <string>
#include <utility>

namespace utilities {

struct LogSource {
    /** Where write_log goes; null: nowhere (an engine on its own, as in a test). */
    Logger* logger = nullptr;
    std::string source_name;

    LogSource() = default;
    LogSource(Logger* logger, std::string source_name)
        : logger(logger), source_name(std::move(source_name)) {}

    /** Log msg as source; source empty => source_name. */
    void write_log(const std::string& msg, LogLevel level = INFO,
                   const std::string& source = "") const;
    /** Whether write_log at level would be kept: skip formatting a message that would not. */
    [[nodiscard]] bool enabled(LogLevel level) const {
        return logger != nullptr && logger->enabled(level);
    }
};

} // namespace utilities
