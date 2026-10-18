#pragma once

/**
 * LogEngine: the Logger, writing on its own log thread. Callers push pooled records into a
 * lock-free ring and never make a system call: the log thread polls every kWriteInterval and is
 * woken only past kWakeBacklog. It writes to the sink and keeps the latest kStreamRingCap for gRPC
 * StreamLogs. When the queue is full new records are dropped and counted.
 */

#include "utilities/log_level.hpp"
#include "utilities/logger.hpp"
#include "utilities/mpsc_ring.hpp"
#include "model/object.hpp"
#include "utilities/object_pool.hpp"
#include "utilities/wakeup.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace core {

/** Where records are written; default stdout. Called on the log thread. */
using LogSink = std::function<void(const utilities::LogRecord&)>;

class LogEngine final : public utilities::Logger {
  public:
    static constexpr std::size_t kStreamRingCap = 1024;
    static constexpr std::size_t kQueueCap = 8192;
    static constexpr std::chrono::milliseconds kWriteInterval{2};
    /** Backlog at which a caller wakes the log thread. */
    static constexpr std::uint64_t kWakeBacklog = kQueueCap / 4;

    LogEngine() = default;
    explicit LogEngine(utilities::LogLevel level) : level_(level) {}
    /** Writes what is queued, then stops the log thread. */
    ~LogEngine() override;

    LogEngine(const LogEngine&) = delete;
    LogEngine& operator=(const LogEngine&) = delete;

    /** Set before anything logs. */
    void set_sink(LogSink sink) { sink_ = std::move(sink); }
    /** The time a record gets when it has none (a backtest's: the data's); default the system
     * clock. Called on the logging thread. Set before anything logs. */
    void set_clock(std::function<std::chrono::system_clock::time_point()> clock) {
        clock_ = std::move(clock);
    }
    void set_level(utilities::LogLevel level) { level_ = level; }
    [[nodiscard]] utilities::LogLevel level() const { return level_; }

    /** Any thread, non-blocking. The first record starts the log thread. */
    void log(const utilities::LogRecord& record) override;
    void log(utilities::LogLevel level, std::string_view source, std::string_view msg) override;
    [[nodiscard]] bool enabled(utilities::LogLevel level) const override {
        return level >= level_;
    }
    /** Blocks until every earlier record is written and streamed. */
    void flush();

    /** False after timeout_ms without a record. */
    bool pop_log_for_stream(utilities::LogRecord& out, int timeout_ms);

  private:
    /** A pooled record to fill; null when the level is filtered or the pool is closing. */
    utilities::LogRecord* acquire(utilities::LogLevel level);
    void run();
    /** Local time text, cached per second (log thread). */
    void stamp(utilities::LogRecord& record);
    void write(const utilities::LogRecord& record);
    /** Takes ownership of the pooled record. */
    void stream(utilities::LogRecord* record);
    /** p is from log_pool_; its time is set here if unset. */
    void enqueue(utilities::LogRecord* p);

    std::atomic<utilities::LogLevel> level_{utilities::INFO};
    LogSink sink_;
    std::function<std::chrono::system_clock::time_point()> clock_;
    utilities::ObjectPool<utilities::LogRecord> log_pool_;

    utilities::MpscRing<utilities::LogRecord*, kQueueCap> queue_;
    utilities::Wakeup wakeup_; // the log thread sleeps here
    std::once_flag thread_started_;
    std::atomic<bool> stopping_{false};
    std::time_t stamped_second_ = -1; // stamp()'s cache (log thread)
    std::string stamped_text_;
    std::atomic<std::uint64_t> queued_{0};
    std::atomic<std::uint64_t> written_{0}; // written and streamed
    std::atomic<std::uint64_t> dropped_{0}; // queue full
    std::thread thread_;

    utilities::MpscRing<utilities::LogRecord*, kStreamRingCap> stream_ring_;
    std::mutex stream_mutex_; // the ring's consumer side: the stream, or the log thread evicting
    std::condition_variable stream_cv_;
};

} // namespace core
