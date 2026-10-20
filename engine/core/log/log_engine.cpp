/** LogEngine: filter and stamp on the caller; write and stream on the log thread. */

#include "core/log/log_engine.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>

namespace core {

namespace {

/** yy-mm-dd HH:MM:SS, local time. */
auto local_time(std::time_t t) -> std::string {
    std::tm local{};
    localtime_r(&t, &local); // std::localtime's buffer is shared between threads
    std::array<char, 32> text{};
    return {text.data(), std::strftime(text.data(), text.size(), "%y-%m-%d %H:%M:%S", &local)};
}

} // namespace

LogEngine::~LogEngine() {
    stopping_.store(true, std::memory_order_release);
    wakeup_.wake();
    if (thread_.joinable()) {
        thread_.join(); // it writes what is queued first
    }
    if (rings_ == nullptr) {
        return; // nothing ever logged
    }
    utilities::LogRecord* p = nullptr;
    while (rings_->queue.try_pop(p)) { // logged as it stopped
        log_pool_.release(p);
    }
    while (rings_->stream.try_pop(p)) {
        log_pool_.release(p);
    }
}

auto LogEngine::acquire(utilities::LogLevel level) -> utilities::LogRecord* {
    if (level < level_) {
        return nullptr;
    }
    return log_pool_.take(); // null when the pool is closing
}

void LogEngine::log(const utilities::LogRecord& record) {
    utilities::LogRecord* p = acquire(record.level);
    if (p == nullptr) {
        return;
    }
    *p = record; // into the pooled record's buffers: nothing allocated once warm
    enqueue(p);
}

void LogEngine::log(utilities::LogLevel level, std::string_view source, std::string_view msg) {
    utilities::LogRecord* p = acquire(level);
    if (p == nullptr) {
        return;
    }
    p->source.assign(source); // the pooled record's buffers, reused
    p->msg.assign(msg);
    p->level = level;
    p->time.clear();
    p->at = {};
    enqueue(p);
}

void LogEngine::enqueue(utilities::LogRecord* p) {
    if (p->time.empty() && p->at == std::chrono::system_clock::time_point{}) {
        p->at = clock_ ? clock_() : std::chrono::system_clock::now(); // the text is the log thread's
    }
    std::call_once(thread_started_, [this] {
        rings_ = std::make_unique<Rings>();
        started_.store(rings_.get(), std::memory_order_release);
        thread_ = std::thread([this] { run(); });
    });
    if (!rings_->queue.try_push(p)) {
        log_pool_.release(p);
        ++dropped_; // the log thread says how many
        wakeup_.wake();
        return;
    }
    // The log thread comes by every kWriteInterval: woken only when much is waiting.
    if (queued_.fetch_add(1, std::memory_order_relaxed) + 1 -
            written_.load(std::memory_order_relaxed) >= kWakeBacklog) {
        wakeup_.wake();
    }
}

void LogEngine::flush() {
    const std::uint64_t target = queued_;
    wakeup_.wake();
    while (written_.load(std::memory_order_acquire) < target) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

void LogEngine::run() {
    std::uint64_t reported = 0; // drops told so far
    for (;;) {
        // Until the next write, or sooner when woken (a backlog, a flush, the stop).
        wakeup_.wait_until(utilities::Wakeup::Clock::now() + kWriteInterval, [this] {
            return stopping_.load(std::memory_order_relaxed) ||
                   queued_.load(std::memory_order_relaxed) -
                           written_.load(std::memory_order_relaxed) >= kWakeBacklog;
        });
        const bool stopping = stopping_.load(std::memory_order_acquire);
        bool wrote = false;
        utilities::LogRecord* p = nullptr;
        while (rings_->queue.try_pop(p)) {
            stamp(*p);
            write(*p);
            stream(p);
            written_.fetch_add(1, std::memory_order_release);
            wrote = true;
        }
        if (const std::uint64_t dropped = dropped_; dropped > reported) {
            utilities::LogRecord note;
            note.source = "Log";
            note.msg = std::format("{} log records dropped: {} were waiting to be written",
                                   dropped - reported, kQueueCap);
            note.level = utilities::WARNING;
            note.at = std::chrono::system_clock::now();
            stamp(note);
            reported = dropped;
            write(note);
            wrote = true;
        }
        if (wrote && !sink_) {
            std::fflush(stdout); // stdout to a file is block-buffered: written once a batch
        }
        if (stopping && rings_->queue.empty()) {
            return;
        }
    }
}

void LogEngine::stamp(utilities::LogRecord& record) {
    if (!record.time.empty()) {
        return;
    }
    const std::time_t second = std::chrono::system_clock::to_time_t(record.at);
    if (second != stamped_second_) {
        stamped_second_ = second;
        stamped_text_ = local_time(second);
    }
    record.time = stamped_text_;
}

void LogEngine::write(const utilities::LogRecord& record) {
    if (sink_) {
        sink_(record);
        return;
    }
    std::println(stdout, "{} | {} | {} | {}", record.time, utilities::level_to_string(record.level),
                 record.source, record.msg);
}

void LogEngine::stream(utilities::LogRecord* record) {
    auto& ring = rings_->stream;
    if (!ring.try_push(record)) {
        // Full, as when nothing reads the stream: drop the oldest, so it holds the latest.
        std::scoped_lock lock(stream_mutex_);
        utilities::LogRecord* oldest = nullptr;
        if (ring.try_pop(oldest)) {
            log_pool_.release(oldest);
        }
        if (!ring.try_push(record)) {
            log_pool_.release(record);
            return;
        }
    }
    {
        std::scoped_lock lock(stream_mutex_); // a reader between its check and its wait still wakes
    }
    stream_cv_.notify_one();
}

auto LogEngine::pop_log_for_stream(utilities::LogRecord& out, int timeout_ms) -> bool {
    std::unique_lock lock(stream_mutex_);
    Rings* rings = nullptr;
    if (!stream_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, &rings] {
            rings = started_.load(std::memory_order_acquire);
            return rings != nullptr && !rings->stream.empty();
        })) {
        return false;
    }
    utilities::LogRecord* p = nullptr;
    if (!rings->stream.try_pop(p) || p == nullptr) {
        return false;
    }
    lock.unlock();
    out = std::move(*p);
    log_pool_.release(p);
    return true;
}

} // namespace core
