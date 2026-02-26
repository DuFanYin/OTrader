/**
 * Log engine (shared): consumes LogIntent via process_log_intent (no event subscription).
 */

#include "engine_log.hpp"
#include "log_sink.hpp"

namespace engines {

LogEngine::LogEngine(utilities::MainEngine* main_engine) : BaseEngine(main_engine, "log") {
    active_ = true;
}

void LogEngine::process_log_intent(const utilities::LogData& data) {
    if (!active_ || data.level < level_) {
        return;
    }
    if (sink_) {
        utilities::LogData copy = data;
        if (copy.time.empty()) {
            copy.time = log_sink::format_time();
        }
        sink_(copy);
    } else {
        log_sink::default_sink(data);
    }
}

} // namespace engines
