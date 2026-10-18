#pragma once

#include "core/gateway/gateway.hpp"
#include "core/main.hpp"
#include "core/market_data/market_data_engine.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace live {

/** IbGateway from the environment with BUILD_GATEWAY, else the base core::Gateway (rejects orders). */
std::unique_ptr<core::Gateway> make_gateway(utilities::Logger* logger);

using Synced = core::Main::Synced;
using Clock = std::function<model::DateTime()>;
using MakeGateway = std::function<std::unique_ptr<core::Gateway>(
    utilities::Logger*, const model::Portfolios&)>;

/**
 * The live core::Main: wall clock, SQLite at $OPTRA_DB, the build's market data, event loop on its own
 * thread ($OPTRA_EVENT_SPIN_US, $OPTRA_EVENT_CPU). Restores contracts, strategies, orders and
 * fills, but talks to nothing outside until TradingDays starts. $OPTRA_CALLBACK_BUDGET_US: stop a strategy
 * whose callbacks keep overrunning; $OPTRA_BROKER_CHECK=warn: start strategies despite position
 * mismatch.
 */
std::unique_ptr<core::Main> make_main(std::unique_ptr<core::Gateway> gateway = nullptr);
/** Custom venue, quotes and clock (simulated market: entry_simulate). */
std::unique_ptr<core::Main> make_main(const MakeGateway& make_gateway,
                                      std::unique_ptr<core::MarketData> market_data, Clock clock);

/**
 * Schedules the live trading days around a core::Main that outlives it: start() connects and begins the
 * day, then a thread rolls in each day's contracts at kRollTime. Each step connects first when the
 * broker is not connected, so an engine started before its gateway (or while it cannot log in)
 * joins it within a minute of its coming up; a connection lost later is the gateway's own to
 * restore. stop() leaves strategies as they are so they resume on the next start. The work itself
 * is core::Main's; this only keeps time.
 */
class TradingDays {
  public:
    /** retry: between attempts to connect or to roll a day in (a test's shorter). */
    explicit TradingDays(core::Main& main, std::chrono::milliseconds retry = std::chrono::minutes(1))
        : main_(main), retry_(retry) {}
    ~TradingDays() { stop(); }

    TradingDays(const TradingDays&) = delete;
    TradingDays& operator=(const TradingDays&) = delete;

    /** New York time, before the open. */
    static constexpr std::chrono::minutes kRollTime{8 * 60 + 45};

    /** Once only. Begins the day before returning; if it cannot connect or the contracts do not
     * come in, nothing starts and the thread retries each minute. */
    void start(std::vector<Synced> portfolios, bool resume);
    /** Reverse of start(); a roll under way is not waited for: the broker is disconnected first,
     * so its sync fails and nothing of it is rolled in. Idempotent. */
    void stop();
    /** From any thread, while start() may still be beginning the day: a sync under way fails at
     * once (the broker disconnected), so that a stop is never held up by one. */
    void interrupt();

  private:
    /** Connected to the broker, connecting (and catching up) now if not. A failure is logged once
     * until it connects. */
    bool connected();
    /** False (logged, nothing started) when not connected or the contracts did not come in. */
    bool begin_day(std::span<const Synced> portfolios, bool resume);
    /** Begins the first day if start() could not, then rolls at kRollTime. */
    void run_days(std::stop_token stop, std::vector<Synced> portfolios, bool resume, bool begun);

    core::Main& main_;
    std::chrono::milliseconds retry_;
    bool started_ = false;        // start() called: a run is once
    bool connect_failed_ = false; // since the last connect: said once
    std::jthread thread_;  // run_days; last: stopped first
};

} // namespace live
