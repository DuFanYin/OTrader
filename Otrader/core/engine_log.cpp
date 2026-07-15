/**
 * Log engine (shared): consumes LogIntent via process_log_intent (no event subscription).
 * write_log builds LogData, pushes to stream buffer, and processes intent.
 */

#include "engine_log.hpp"
#include <chrono>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace engines {

namespace {

auto format_time() -> std::string {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream os;
    // Format: yy-mm-dd HH:MM:SS
    os << std::put_time(std::localtime(&t), "%y-%m-%d %H:%M:%S");
    return os.str();
}

void default_sink(const utilities::LogData& log) {
    const std::string ts = log.time.empty() ? format_time() : log.time;
    std::cout << std::format("{} | {} | {} | {}\n", ts, level_to_string(log.level),
                             log.gateway_name, log.msg);
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

LogEngine::LogEngine(runtime_common::MainEngineBase* main_engine)
    : BaseEngine(main_engine, "log") {
    active_ = true;
}

void LogEngine::process_log_intent(const utilities::LogData& data) {
    if (!active_ || data.level < level_) {
        return;
    }
    utilities::LogData* p = log_pool_.acquire();
    if (p == nullptr) {
        utilities::LogData tmp = data;
        if (tmp.time.empty()) {
            tmp.time = format_time();
        }
        if (sink_) {
            sink_(tmp);
        } else {
            default_sink(tmp);
        }
        return;
    }
    *p = data;
    if (p->time.empty()) {
        p->time = format_time();
    }
    if (!stream_ring_.try_push(p)) {
        log_pool_.release(p);
    } else {
        stream_cv_.notify_one();
    }
    if (sink_) {
        sink_(*p);
    } else {
        default_sink(*p);
    }
}

auto LogEngine::pop_log_for_stream(utilities::LogData& out, int timeout_ms) -> bool {
    std::unique_lock<std::mutex> lk(stream_mutex_);
    if (!stream_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                             [this]() -> bool { return !stream_ring_.empty(); })) {
        return false;
    }
    utilities::LogData* p = nullptr;
    if (!stream_ring_.try_pop(p) || p == nullptr) {
        return false;
    }
    lk.unlock();
    out = *p;
    log_pool_.release(p);
    return true;
}

} // namespace engines
