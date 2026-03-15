#pragma once

/** LogEngine: process_log_intent, format, stdout sink. */

#include "../utilities/base_engine.hpp"
#include "../utilities/mpsc_ring.hpp"
#include "../utilities/object.hpp"
#include "../utilities/object_pool.hpp"
#include <condition_variable>
#include <cstdint>
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
/** Disable all output. */
inline constexpr int DISABLED = 99;

/** Level to string. */
std::string level_to_string(int level);

/** Log sink; default stdout. */
using LogSink = std::function<void(const utilities::LogData&)>;

class LogEngine : public utilities::BaseEngine {
  public:
    explicit LogEngine(utilities::MainEngine* main_engine);

    void set_sink(LogSink sink) { sink_ = std::move(sink); }
    void set_active(bool active) { active_ = active; }
    /** Output when level >= threshold; DISABLED = suppress. */
    void set_level(int level) { level_ = level; }
    int level() const { return level_; }

    void process_log_intent(const utilities::LogData& data);

    /** Pop log for gRPC stream. */
    bool pop_log_for_stream(utilities::LogData& out, int timeout_ms);

    /** Object pool for LogData: callers may acquire, fill, pass to process_log_intent(*p), then
     * release. */
    utilities::LogData* acquire_log() { return log_pool_.acquire(); }
    void release_log(utilities::LogData* p) { log_pool_.release(p); }

  private:
    static constexpr size_t kStreamRingCap = 1024;

    utilities::ObjectPool<utilities::LogData> log_pool_;
    bool active_ = true;
    int level_ = INFO;
    LogSink sink_;
    utilities::MpscRing<utilities::LogData*, kStreamRingCap> stream_ring_;
    std::mutex stream_mutex_;
    std::condition_variable stream_cv_;
};

} // namespace engines
