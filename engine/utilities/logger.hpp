#pragma once

/** Logger: where log records go. The running engine's LogEngine (core/log/log_engine.hpp) is the
 * one; engines write to it through LogSource::write_log, strategies through their StrategyContext. */

#include "utilities/log_level.hpp"
#include <chrono>
#include <string>
#include <string_view>

namespace utilities {

/** A log record: who wrote it (source), what, how important, when. */
struct LogRecord {
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
    virtual void log(const LogRecord& record) = 0;
    /** The same record from its parts, without a LogRecord to build first (what write_log calls).
     * By default it builds one; LogEngine writes the parts straight into its queued record. */
    virtual void log(LogLevel level, std::string_view source, std::string_view msg) {
        LogRecord record;
        record.source = source;
        record.msg = msg;
        record.level = level;
        log(record);
    }
    /** Whether a record at level would be kept: callers skip formatting one that would not. */
    [[nodiscard]] virtual bool enabled(LogLevel /*level*/) const { return true; }
};

} // namespace utilities
