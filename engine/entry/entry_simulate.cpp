/**
 * entry_simulate: the live engine on a simulated market, served over gRPC as entry_live serves it.
 * No broker: recorded days (the backtest's Parquet) are replayed as the market from their first
 * minute at the wall clock's pace (adapters::ReplayFeed), and orders fill at their quotes in a
 * simulated account (adapters::SimulatedGateway). The engine itself is the live one, unchanged:
 * this file only chooses its venue, its quotes and its clock. Used by the public demo.
 */

#include "adapters/backtest/simulation.hpp"
#include "adapters/db/engine_db_sqlite.hpp"
#include "core/engine_live.hpp"
#include "core/engines/log.hpp"
#include "entry/engine_grpc.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <print>
#include <pthread.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace {

/** Loopback only: the gRPC service has no authentication and can place orders. */
constexpr const char* kDefaultListenAddress = "localhost:50051";
/** After the last quote, the engine keeps serving this long, then stops. */
constexpr auto kAfterSession = std::chrono::minutes(15);
constexpr double kFeeRate = 0.35;     // USD a contract, as the backtest's default
constexpr double kSlippageBps = 5.0;

struct SimulateOptions {
    std::string listen_address = kDefaultListenAddress;
    /** Once serving: connect, start the market data and the strategies that were running. */
    bool resume = false;
    std::vector<std::string> days;
};

void print_usage(std::FILE* out) {
    std::print(out,
               "Usage: entry_simulate [--listen ADDRESS] [--resume] DAY.parquet...\n"
               "  Runs the live engine on a simulated market, served like entry_live: the days\n"
               "  (the backtest's Parquet, under data/<PORTFOLIO>/; one per portfolio) replayed\n"
               "  from their first minute at the wall clock's pace, orders filled at their quotes.\n"
               "  The contracts come from the files; the account is kept in $OPTRA_DB.account.json.\n"
               "  Stops on SIGINT / SIGTERM, or 15 minutes after the last quote.\n"
               "\n"
               "  --listen ADDRESS  gRPC address (default {}; env LIVE_GRPC_ADDRESS)\n"
               "  --resume          once serving: connect, start the market data and the\n"
               "                    strategies that were running\n"
               "  -h, --help        this help\n",
               kDefaultListenAddress);
}

/** The days' contracts into the database, as a sync from a broker would put them. */
auto sync_contracts(const std::vector<std::string>& days) -> bool {
    try {
        core::LogEngine log;
        adapters::DatabaseEngine db(&log);
        for (const std::string& portfolio : adapters::simulated_portfolios(days)) {
            db.save_contracts(adapters::simulated_contracts(days, portfolio));
        }
        db.close();
        return true;
    } catch (const std::exception& e) {
        std::println(stderr, "The days' contracts: {}", e.what());
        return false;
    }
}

auto account_file() -> std::string {
    const char* db = std::getenv("OPTRA_DB");
    return std::string(db != nullptr && *db != '\0' ? db : "data/optra.db") + ".account.json";
}

/** Serves the gRPC EngineService until SIGINT / SIGTERM or the session's end, then closes the
 * engine. */
auto run(const SimulateOptions& options) -> int {
    // As entry_live: the stop signals are blocked in every thread and taken by the stopper, which
    // here also stops the run once the session is over.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    std::pair<model::DateTime, model::DateTime> session;
    try {
        session = adapters::simulated_session(options.days);
    } catch (const std::exception& e) {
        std::println(stderr, "The days: {}", e.what());
        return 1;
    }
    const auto lasts = session.second - session.first + kAfterSession;

    static std::mutex serving_mutex;
    static grpc::Server* serving = nullptr;
    std::thread stopper([stop_signals, lasts] {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(lasts).count();
        const timespec timeout{.tv_sec = static_cast<time_t>(seconds), .tv_nsec = 0};
        const int signal = sigtimedwait(&stop_signals, nullptr, &timeout);
        const std::lock_guard lock(serving_mutex);
        if (serving == nullptr) {
            std::println("Stopping before the engine serves");
            std::fflush(stdout);
            std::_Exit(signal > 0 ? 128 + signal : 0);
        }
        if (signal > 0) {
            std::println("Signal {}: stopping", signal);
        } else {
            std::println("The simulated session is over: stopping");
        }
        std::fflush(stdout);
        serving->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    });

    if (!sync_contracts(options.days)) {
        stopper.detach();
        return 1;
    }

    // The clock starts at the days' first quote and keeps the wall clock's pace; the venue and the
    // replay read the same clock as the engine.
    const auto offset = std::chrono::system_clock::now() - session.first;
    const core::live::MainEngine::Clock clock = [offset] {
        return std::chrono::system_clock::now() - offset;
    };
    adapters::SimulatedGateway* venue = nullptr;
    core::live::MainEngine* engine = nullptr; // set before the replay can start (resume, or a call)
    // After each minute's quotes: the orders they fill, on the event thread like every booking.
    auto feed = std::make_unique<adapters::ReplayFeed>(options.days, clock, [&venue, &engine] {
        try {
            engine->on_event_thread([&venue] { venue->on_minute(); });
        } catch (const std::exception&) { // the engine is stopping
        }
    });
    std::unique_ptr<core::live::MainEngine> main_engine;
    try {
        main_engine = std::make_unique<core::live::MainEngine>(
            [&](const model::PortfolioStructure& portfolios) {
                auto gateway = std::make_unique<adapters::SimulatedGateway>(
                    portfolios, options.days, account_file(), clock, kFeeRate, kSlippageBps);
                venue = gateway.get();
                return gateway;
            },
            std::move(feed), clock);
    } catch (const std::exception& e) {
        std::println(stderr, "The engine did not start: {}", e.what());
        stopper.detach();
        return 1;
    }
    engine = main_engine.get();
    engine->write_log("Simulated market: no broker; quotes replayed, orders filled at them",
                      utilities::WARNING);

    entry::GrpcLiveEngineService service(*engine);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(options.listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::println(stderr, "Failed to start gRPC server on {}", options.listen_address);
        stopper.detach();
        return 1;
    }
    {
        const std::lock_guard lock(serving_mutex);
        serving = server.get();
    }

    std::println("Simulated engine listening on {}", options.listen_address);
    std::fflush(stdout);
    if (options.resume) {
        engine->resume();
    }
    server->Wait();
    stopper.join();

    try {
        engine->disconnect();
        engine->close();
    } catch (...) {
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    SimulateOptions options;
    const char* env_address = std::getenv("LIVE_GRPC_ADDRESS");
    if (env_address != nullptr && *env_address != '\0') {
        options.listen_address = env_address;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(stdout);
            return 0;
        }
        if (arg == "--listen" && i + 1 < argc) {
            options.listen_address = argv[++i];
            continue;
        }
        if (arg == "--resume") {
            options.resume = true;
            continue;
        }
        if (arg.starts_with("-")) {
            std::println(stderr, "entry_simulate: unknown argument '{}'\n", arg);
            print_usage(stderr);
            return 2;
        }
        options.days.emplace_back(arg);
    }
    if (options.days.empty()) {
        print_usage(stderr);
        return 2;
    }
    return run(options);
}
