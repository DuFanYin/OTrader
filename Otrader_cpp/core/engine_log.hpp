#pragma once

/**
 * Log engine (shared): consumes LogIntent via process_log_intent (no event subscription).
 * Sink stays here (set_sink / default_sink). EventEngine routes log intent into LogEngine (no
 * context interface).
 */

#include "../utilities/base_engine.hpp"
#include "../utilities/object.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace engines {

// Log levels (match Python logging)
inline constexpr int DEBUG = 10;
inline constexpr int INFO = 20;
inline constexpr int WARNING = 30;
inline constexpr int ERROR = 40;
inline constexpr int CRITICAL = 50;
/** Set as min level to disable all output (no message has level >= this). */
inline constexpr int DISABLED = 99;

/** Optional sink for log output; if not set, default_sink (stdout) is used. */
using LogSink = std::function<void(const utilities::LogData&)>;

class LogEngine : public utilities::BaseEngine {
  public:
    explicit LogEngine(utilities::MainEngine* main_engine);

    void set_sink(LogSink sink) { sink_ = std::move(sink); }
    void set_active(bool active) { active_ = active; }
    /** Only output when data.level >= level. Use DISABLED to suppress all. */
    void set_level(int level) { level_ = level; }
    int level() const { return level_; }

    /** Consume LogIntent (EventEngine routes here). */
    void process_log_intent(const utilities::LogData& data);

  private:
    bool active_ = true;
    int level_ = INFO; // only output when data.level >= level_
    LogSink sink_;
};

} // namespace engines
