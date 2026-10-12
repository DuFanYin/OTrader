/** LogEngine: filter and stamp on the caller; write and stream on the log thread. */

#include "core/engines/log.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <print>
#include <string>
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
    utilities::LogData* p = nullptr;
    while (queue_.try_pop(p)) { // logged as it stopped
        log_pool_.release(p);
    }
    while (stream_ring_.try_pop(p)) {
        log_pool_.release(p);
    }
}

void LogEngine::log(const utilities::LogData& record) {
    if (!active_ || record.level < level_) {
        return;
    }
    utilities::LogData* p = log_pool_.acquire();
    if (p == nullptr) {
        return; // the pool is closing
    }
    *p = record; // into the pooled record's buffers: nothing allocated once warm
    if (p->time.empty() && p->at == std::chrono::system_clock::time_point{}) {
        p->at = std::chrono::system_clock::now(); // the text is the log thread's to write
    }
    std::call_once(thread_started_, [this] { thread_ = std::thread([this] { run(); }); });
    if (!queue_.try_push(p)) {
        log_pool_.release(p);
        ++dropped_; // the log thread says how many
        return;
    }
    ++queued_;
    wakeup_.wake();
}

void LogEngine::flush() {
    const std::uint64_t target = queued_;
    while (written_.load(std::memory_order_acquire) < target) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

void LogEngine::run() {
    std::uint64_t reported = 0; // drops told so far
    for (;;) {
        wakeup_.wait_until(utilities::Wakeup::Clock::time_point::max(), [this] {
            return stopping_.load(std::memory_order_relaxed) || !queue_.empty();
        });
        const bool stopping = stopping_.load(std::memory_order_acquire);
        bool wrote = false;
        utilities::LogData* p = nullptr;
        while (queue_.try_pop(p)) {
            stamp(*p);
            write(*p);
            stream(p);
            written_.fetch_add(1, std::memory_order_release);
            wrote = true;
        }
        if (const std::uint64_t dropped = dropped_; dropped > reported) {
            utilities::LogData note;
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
        if (stopping && queue_.empty()) {
            return;
        }
    }
}

void LogEngine::stamp(utilities::LogData& record) {
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

void LogEngine::write(const utilities::LogData& record) {
    if (sink_) {
        sink_(record);
        return;
    }
    std::println(stdout, "{} | {} | {} | {}", record.time, utilities::level_to_string(record.level),
                 record.source, record.msg);
}

void LogEngine::stream(utilities::LogData* record) {
    if (!stream_ring_.try_push(record)) {
        // Full, as when nothing reads the stream: drop the oldest, so it holds the latest.
        std::scoped_lock lock(stream_mutex_);
        utilities::LogData* oldest = nullptr;
        if (stream_ring_.try_pop(oldest)) {
            log_pool_.release(oldest);
        }
        if (!stream_ring_.try_push(record)) {
            log_pool_.release(record);
            return;
        }
    }
    {
        std::scoped_lock lock(stream_mutex_); // a reader between its check and its wait still wakes
    }
    stream_cv_.notify_one();
}

auto LogEngine::pop_log_for_stream(utilities::LogData& out, int timeout_ms) -> bool {
    std::unique_lock lock(stream_mutex_);
    if (!stream_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] { return !stream_ring_.empty(); })) {
        return false;
    }
    utilities::LogData* p = nullptr;
    if (!stream_ring_.try_pop(p) || p == nullptr) {
        return false;
    }
    lock.unlock();
    out = std::move(*p);
    log_pool_.release(p);
    return true;
}

} // namespace core
