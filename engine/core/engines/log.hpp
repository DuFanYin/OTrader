#pragma once

/**
 * LogEngine: the engine's Logger. A record at or above the level is stamped with the time on the
 * thread that logs it and queued for the log thread (a pooled copy into a lock-free ring, and a
 * wake-up that costs a system call only while the log thread sleeps); the log thread turns the
 * time into local time, writes the record to the sink (stdout by default) and keeps it for the log
 * stream (gRPC StreamLogs; the latest kStreamRingCap). No caller waits on the output: when
 * kQueueCap records are waiting, a new one is dropped and counted, and the log thread says how
 * many.
 */

#include "utilities/log_level.hpp"
#include "utilities/logger.hpp"
#include "utilities/mpsc_ring.hpp"
#include "model/object.hpp"
#include "utilities/object_pool.hpp"
#include "utilities/wakeup.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace core {

/** Where records are written; default stdout. Called on the log thread. */
using LogSink = std::function<void(const utilities::LogData&)>;

class LogEngine final : public utilities::Logger {
  public:
    /** How many records the stream keeps. */
    static constexpr std::size_t kStreamRingCap = 1024;
    /** How many records may wait for the log thread. */
    static constexpr std::size_t kQueueCap = 8192;

    LogEngine() = default;
    /** Writes what is queued, then stops the log thread. */
    ~LogEngine() override;

    LogEngine(const LogEngine&) = delete;
    LogEngine& operator=(const LogEngine&) = delete;

    /** Set before anything logs. */
    void set_sink(LogSink sink) { sink_ = std::move(sink); }
    void set_active(bool active) { active_ = active; }
    /** Output records at or above level; DISABLED: none. */
    void set_level(utilities::LogLevel level) { level_ = level; }
    [[nodiscard]] utilities::LogLevel level() const { return level_; }

    /** Any thread, never waiting on the output: kept for the log thread (started by the first
     * record), which writes the records in the order they were logged. */
    void log(const utilities::LogData& record) override;
    /** Wait until every record logged before has been written and streamed. */
    void flush();

    /** The oldest record the stream has not given out; false after timeout_ms without one. */
    bool pop_log_for_stream(utilities::LogData& out, int timeout_ms);

  private:
    /** The log thread: write and stream what is queued, until stopped and empty. */
    void run();
    /** The record's time as local time text, when it has none (log thread). */
    void stamp(utilities::LogData& record);
    void write(const utilities::LogData& record);
    /** Keep the pooled record for the log stream (it owns it from now on). */
    void stream(utilities::LogData* record);

    std::atomic<bool> active_{true};
    std::atomic<utilities::LogLevel> level_{utilities::INFO};
    LogSink sink_;
    utilities::ObjectPool<utilities::LogData> log_pool_;

    utilities::MpscRing<utilities::LogData*, kQueueCap> queue_; // to the log thread
    utilities::Wakeup wakeup_; // the log thread sleeps here
    std::once_flag thread_started_;
    std::atomic<bool> stopping_{false};
    std::time_t stamped_second_ = -1; // the last second stamp() wrote, and its text (log thread)
    std::string stamped_text_;
    std::atomic<std::uint64_t> queued_{0};  // records queued so far
    std::atomic<std::uint64_t> written_{0}; // of those, written and streamed
    std::atomic<std::uint64_t> dropped_{0}; // records the full queue turned away
    std::thread thread_;

    utilities::MpscRing<utilities::LogData*, kStreamRingCap> stream_ring_;
    std::mutex stream_mutex_; // the ring's consumer side: the stream, or the log thread evicting
    std::condition_variable stream_cv_;
};

} // namespace core
