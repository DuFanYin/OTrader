#include <chrono>
#include <csignal>
#include <cstring>
#include <gtest/gtest.h>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include "runtime/live/engine_event.hpp"
#include "runtime/live/market_data_client.hpp"

namespace {

template <typename Fn>
void run_in_subprocess_with_timeout(const char* step_name, std::chrono::milliseconds timeout,
                                    Fn&& fn) {
    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1) << "fork() failed: " << std::strerror(errno);

    if (pid == 0) {
        // Child process: run the step, then exit.
        try {
            fn();
            std::_Exit(0);
        } catch (...) {
            std::_Exit(101);
        }
    }

    // Parent process: wait with timeout.
    auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (true) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r == -1) {
            FAIL() << step_name << " waitpid() failed: " << std::strerror(errno);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            (void)::waitpid(pid, &status, 0);
            FAIL() << step_name << " timed out after " << timeout.count() << "ms";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (WIFSIGNALED(status)) {
        FAIL() << step_name << " died from signal " << WTERMSIG(status);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAIL() << step_name << " failed with exit code "
               << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
}

TEST(LiveClose, EventEngine_Start_Returns) {
    run_in_subprocess_with_timeout("EventEngine::start", std::chrono::milliseconds(300), [] {
        engines::EventEngine ee(nullptr /*main*/, 1);
        ee.start();
        ee.stop();
    });
}

TEST(LiveClose, EventEngine_Stop_Returns) {
    run_in_subprocess_with_timeout("EventEngine::stop", std::chrono::milliseconds(300), [] {
        engines::EventEngine ee(nullptr /*main*/, 1);
        ee.start();
        ee.stop();
    });
}

TEST(LiveClose, MarketDataClient_Start_Returns_NoExternal) {
    run_in_subprocess_with_timeout("MarketDataClient::start", std::chrono::milliseconds(300), [] {
        engines::MarketDataClient md(nullptr /*main*/);
        md.set_reqrep_timeout_ms(0);
        md.set_sub_rcv_timeout_ms(0);
        md.start();
        md.stop(/*send_stop_cmd*/ false);
    });
}

TEST(LiveClose, MarketDataClient_Stop_Returns_NoExternal) {
    run_in_subprocess_with_timeout("MarketDataClient::stop(false)", std::chrono::milliseconds(300),
                                   [] {
                                       engines::MarketDataClient md(nullptr /*main*/);
                                       md.set_reqrep_timeout_ms(0);
                                       md.set_sub_rcv_timeout_ms(0);
                                       md.start();
                                       md.stop(/*send_stop_cmd*/ false);
                                   });
}

TEST(LiveClose, MarketDataClient_Close_Returns_NoExternal) {
    run_in_subprocess_with_timeout("MarketDataClient::close", std::chrono::milliseconds(500), [] {
        engines::MarketDataClient md(nullptr /*main*/);
        md.set_reqrep_timeout_ms(0);
        md.set_sub_rcv_timeout_ms(0);
        md.start();
        md.close();
    });
}

} // namespace
