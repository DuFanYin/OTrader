#pragma once

/** Logger: where log records go. The running engine's LogEngine (core/engines/log.hpp) is the one;
 * engines write to it through BaseEngine::write_log, strategies through their StrategyContext. */

#include "log_level.hpp"
#include <chrono>
#include <string>

namespace utilities {

/** A log record: who wrote it (source), what, how important, when. */
struct LogData {
    std::string source;
    std::string msg;
    LogLevel level = INFO;
    std::string time;                          // when, as text: LogEngine writes it from at
    std::chrono::system_clock::time_point at{}; // when it was logged
};

class Logger {
  public:
    virtual ~Logger() = default;

    /** Called from any thread. */
    virtual void log(const LogData& record) = 0;
};

} // namespace utilities
