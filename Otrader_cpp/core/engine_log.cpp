/**
 * Log engine (shared): consumes LogIntent via process_log_intent (no event subscription).
 * write_log builds LogData, pushes to stream buffer, and processes intent.
 */

#include "engine_log.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace engines {

namespace {

auto format_time() -> std::string {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream os;
    os << std::put_time(std::localtime(&t), "%m-%d %H:%M:%S");
    return os.str();
}

void default_sink(const utilities::LogData& log) {
    std::cout << "| " << level_to_string(log.level) << " | " << log.gateway_name << " | " << log.msg
              << '\n';
}

} // namespace

auto level_to_string(int level) -> std::string {
    if (level <= 10) {
        return "DEBUG";
    }
    if (level <= 20) {
        return "INFO";
    }
    if (level <= 30) {
        return "WARNING";
    }
    if (level <= 40) {
        return "ERROR";
    }
    return "CRITICAL";
}

LogEngine::LogEngine(utilities::MainEngine* main_engine) : BaseEngine(main_engine, "log") {
    active_ = true;
}

void LogEngine::write_log(const std::string& msg, int level, const std::string& gateway) {
    utilities::LogData log;
    log.msg = msg;
    log.level = level;
    log.gateway_name = gateway.empty() ? "Main" : gateway;
    log.time = format_time();
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
            copy.time = format_time();
        }
        sink_(copy);
    } else {
        default_sink(data);
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
