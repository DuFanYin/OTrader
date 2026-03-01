#pragma once

/**
 * Log engine (shared): consumes LogIntent via process_log_intent (no event subscription).
 * Formatting and default stdout sink are in engine_log.cpp. EventEngine routes log intent
 * into LogEngine (no context interface).
 */

#include "../utilities/base_engine.hpp"
#include "../utilities/object.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
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

/** Map level number to string (DEBUG=10, INFO=20, WARNING=30, ERROR=40, CRITICAL=50). */
std::string level_to_string(int level);

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

    /** 组帧、写入 stream buffer、并 process_log_intent（供 Main 等直接打 log）。 */
    void write_log(const std::string& msg, int level = INFO, const std::string& gateway = "");

    /** Consume LogIntent (EventEngine routes here). */
    void process_log_intent(const utilities::LogData& data);

    /** 供 gRPC StreamLogs 从 buffer 取 log。 */
    bool pop_log_for_stream(utilities::LogData& out, int timeout_ms);

  private:
    static constexpr size_t kMaxStreamBuffer = 1000;

    bool active_ = true;
    int level_ = INFO; // only output when data.level >= level_
    LogSink sink_;
    std::deque<utilities::LogData> stream_buffer_;
    std::mutex stream_mutex_;
    std::condition_variable stream_cv_;
};

} // namespace engines
