/**
 * Log engine (shared): consumes LogIntent via process_log_intent (no event subscription).
 * write_log builds LogData, pushes to stream buffer, and processes intent.
 */

#include "engine_log.hpp"
#include "log_sink.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace engines {

LogEngine::LogEngine(utilities::MainEngine* main_engine) : BaseEngine(main_engine, "log") {
    active_ = true;
}

void LogEngine::write_log(const std::string& msg, int level, const std::string& gateway) {
    utilities::LogData log;
    log.msg = msg;
    log.level = level;
    log.gateway_name = gateway.empty() ? "Main" : gateway;
    log.time = log_sink::format_time();
    {
        std::scoped_lock lk(stream_mutex_);
        stream_buffer_.push_back(log);
        if (stream_buffer_.size() > kMaxStreamBuffer) {
            stream_buffer_.pop_front();
        }
    }
    stream_cv_.notify_all();
    process_log_intent(log);
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

auto LogEngine::pop_log_for_stream(utilities::LogData& out, int timeout_ms) -> bool {
    std::unique_lock<std::mutex> lk(stream_mutex_);
    if (!stream_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                             [this]() -> bool { return !stream_buffer_.empty(); })) {
        return false;
    }
    out = stream_buffer_.front();
    stream_buffer_.pop_front();
    return true;
}

} // namespace engines
