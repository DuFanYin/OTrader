#pragma once

/**
 * Shared log sink: format and output LogData (used by live LogEngine and backtest EVENT_LOG).
 * No event subscription; caller feeds LogData (e.g. after EVENT_LOG).
 */

#include "../utilities/object.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>

namespace engines {

namespace log_sink {

/** Format current time as "MM-DD HH:MM:SS". */
inline std::string format_time() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream os;
    os << std::put_time(std::localtime(&t), "%m-%d %H:%M:%S");
    return os.str();
}

/** Map level number to string (DEBUG=10, INFO=20, WARNING=30, ERROR=40, CRITICAL=50). */
inline std::string level_to_string(int level) {
    if (level <= 10) return "DEBUG";
    if (level <= 20) return "INFO";
    if (level <= 30) return "WARNING";
    if (level <= 40) return "ERROR";
    return "CRITICAL";
}

/** Default sink: print to stdout as "| LEVEL | gateway_name | msg". */
inline void default_sink(const utilities::LogData& log) {
    std::cout << "| " << level_to_string(log.level) << " | " << log.gateway_name << " | " << log.msg << std::endl;
}

}  // namespace log_sink
}  // namespace engines
