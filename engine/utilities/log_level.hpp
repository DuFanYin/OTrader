#pragma once

/** Log levels (as Python's logging): every engine and strategy logs at one of these. */

#include <string_view>

namespace utilities {

/** A plain enum, so INFO stays the one spelling; no int converts to it, so a level is
 * always one of these. */
enum LogLevel : int {
    DEBUG = 10,
    INFO = 20,
    WARNING = 30,
    ERROR = 40,
    CRITICAL = 50,
    DISABLED = 99, // as a threshold: nothing is output
};

constexpr auto level_to_string(LogLevel level) -> std::string_view {
    switch (level) {
    case DEBUG:
        return "DEBUG";
    case INFO:
        return "INFO";
    case WARNING:
        return "WARNING";
    case ERROR:
        return "ERROR";
    case CRITICAL:
        return "CRITICAL";
    case DISABLED:
        return "DISABLED";
    }
    return "UNKNOWN";
}

} // namespace utilities
