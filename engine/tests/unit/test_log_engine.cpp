/**
 * The log path: an engine logs under its name through its Logger; the LogEngine keeps what is at
 * or above its level, stamps it, and its log thread writes it and streams the latest records.
 */
#include "core/engines/log.hpp"
#include "utilities/base_engine.hpp"
#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Recorder final : utilities::Logger {
    void log(const utilities::LogData& record) override { records.push_back(record); }
    std::vector<utilities::LogData> records;
};

auto record(const std::string& msg, utilities::LogLevel level) -> utilities::LogData {
    utilities::LogData r;
    r.source = "Test";
    r.msg = msg;
    r.level = level;
    return r;
}

TEST(BaseEngineLog, UnderItsNameOrTheSourceGiven) {
    Recorder logger;
    const utilities::BaseEngine engine(&logger, "Position");
    engine.write_log("held", utilities::WARNING);
    engine.write_log("filled", utilities::INFO, "Scalper");

    ASSERT_EQ(logger.records.size(), 2U);
    EXPECT_EQ(logger.records[0].source, "Position");
    EXPECT_EQ(logger.records[0].msg, "held");
    EXPECT_EQ(logger.records[0].level, utilities::WARNING);
    EXPECT_EQ(logger.records[1].source, "Scalper");

    const utilities::BaseEngine alone(nullptr, "Alone");
    alone.write_log("nowhere"); // no logger: nothing happens
}

TEST(LogEngine, KeepsWhatIsAtOrAboveItsLevel) {
    core::LogEngine log;
    std::vector<std::string> printed;
    log.set_sink([&](const utilities::LogData& r) { printed.push_back(r.msg); });

    log.set_level(utilities::WARNING);
    log.log(record("info", utilities::INFO));
    log.log(record("warning", utilities::WARNING));
    log.log(record("error", utilities::ERROR));
    log.set_level(utilities::DISABLED);
    log.log(record("critical", utilities::CRITICAL));
    log.flush(); // written on the log thread

    EXPECT_EQ(printed, (std::vector<std::string>{"warning", "error"}));
}

TEST(LogEngine, StreamsTheLatestStamped) {
    core::LogEngine log;
    log.set_sink([](const utilities::LogData&) {});
    constexpr std::size_t kExtra = 10;
    for (std::size_t i = 0; i < core::LogEngine::kStreamRingCap + kExtra; ++i) {
        log.log(record(std::to_string(i), utilities::INFO));
    }
    log.flush();

    utilities::LogData out;
    ASSERT_TRUE(log.pop_log_for_stream(out, 0));
    EXPECT_EQ(out.msg, std::to_string(kExtra)); // the oldest kExtra made way
    EXPECT_FALSE(out.time.empty());
    std::size_t popped = 1;
    while (log.pop_log_for_stream(out, 0)) {
        ++popped;
    }
    EXPECT_EQ(popped, core::LogEngine::kStreamRingCap);
    EXPECT_EQ(out.msg, std::to_string(core::LogEngine::kStreamRingCap + kExtra - 1));
}

TEST(LogEngine, WritesInTheOrderLoggedFromManyThreads) {
    core::LogEngine log;
    std::vector<std::string> printed; // the sink runs on the log thread alone
    log.set_sink([&](const utilities::LogData& r) { printed.push_back(r.msg); });
    constexpr int kThreads = 4;
    constexpr int kEach = 500;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&log, t] {
                for (int i = 0; i < kEach; ++i) {
                    log.log(record(std::format("{} {}", t, i), utilities::INFO));
                }
            });
        }
    }
    log.flush();

    ASSERT_EQ(printed.size(), static_cast<std::size_t>(kThreads * kEach));
    std::vector<int> next(kThreads, 0); // each thread's records, in the order it logged them
    for (const std::string& msg : printed) {
        const int t = msg[0] - '0';
        EXPECT_EQ(msg, std::format("{} {}", t, next[t]++));
    }
}

TEST(LogLevel, Names) {
    EXPECT_EQ(utilities::level_to_string(utilities::DEBUG), "DEBUG");
    EXPECT_EQ(utilities::level_to_string(utilities::ERROR), "ERROR");
    static_assert(utilities::INFO < utilities::WARNING);
}

} // namespace
