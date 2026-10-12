/**
 * entry_live: the live engine (core::live::MainEngine) in one process, served over gRPC
 * (entry::GrpcLiveEngineService), the control plane's only way in. The market data and the
 * gateway run inside the engine; the gateway is IbGateway or the base Gateway (no broker)
 * depending on BUILD_GATEWAY (core::live::make_gateway), so this builds with or without IBJts.
 */

#include "adapters/db/engine_db_sqlite.hpp"
#include "entry/engine_grpc.hpp"
#include "core/engines/log.hpp"
#include "core/engine_live.hpp"

#include <charconv>
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
constexpr int kDefaultSyncDays = 7;

struct LiveOptions {
    std::string listen_address = kDefaultListenAddress;
    /** Before the engine starts: sync these portfolios' contracts from IB (portfolio, max days
     * to expiry), so that it trades today's chains. */
    std::vector<std::pair<std::string, int>> syncs;
    /** Once serving: connect, start the market data and the strategies that were running. */
    bool resume = false;
};

void print_usage(std::FILE* out) {
    std::print(out,
               "Usage: entry_live [--listen ADDRESS] [--sync PORTFOLIO[:DAYS]]... [--resume]\n"
               "  Runs the live engine: the gRPC EngineService over MainEngine (market data and\n"
               "  gateway in-process). It starts where it stopped: its strategies, inited if they\n"
               "  were, their holdings rebooked from the database. Stops on SIGINT / SIGTERM.\n"
               "\n"
               "  --listen ADDRESS  gRPC address (default: $LIVE_GRPC_ADDRESS, else {}).\n"
               "                    The service is unauthenticated and can trade: listen on\n"
               "                    another interface only behind your own access control.\n"
               "  --sync PORTFOLIO[:DAYS]\n"
               "                    first sync the portfolio's contracts from IB, its options\n"
               "                    expiring within DAYS (default {}); repeat for each portfolio.\n"
               "                    Exits with 1 if a sync fails.\n"
               "  --resume          once serving: connect (positions are checked against IB),\n"
               "                    start the market data and the strategies that were running\n"
               "  -h, --help        this help\n"
               "\n"
               "  Each trading day before the open: stop the engine, then\n"
               "    entry_live --sync SPY --sync SPXW --resume\n"
               "\n"
               "Environment (scripts/dev.sh loads .env): OPTRA_DB (default\n"
               "  data/optra.db), IB_HOST, IB_PORT, IB_CLIENT_ID (orders; quotes connect as\n"
               "  IB_CLIENT_ID + 1), IB_ACCOUNT (required: the one account traded, a paper one,\n"
               "  DU..., unless root names a live one in /etc/optra/live-account; IB's login\n"
               "  must manage it, or nothing connects), IB_MARKET_DATA_LINES (default 100),\n"
               "  IB_MARKET_DATA_TYPE (1 live, 2 frozen, 3 delayed, 4 delayed frozen),\n"
               "  OPTRA_RECONCILE=warn (start strategies even when positions disagree with IB)\n",
               kDefaultListenAddress, kDefaultSyncDays);
}

/** PORTFOLIO[:DAYS]; false when DAYS is not a number of days. */
auto parse_sync(std::string_view arg, LiveOptions& options) -> bool {
    const auto colon = arg.find(':');
    int days = kDefaultSyncDays;
    if (colon != std::string_view::npos) {
        const std::string_view text = arg.substr(colon + 1);
        if (std::from_chars(text.data(), text.data() + text.size(), days).ec != std::errc{} ||
            days < 0) {
            return false;
        }
    }
    options.syncs.emplace_back(std::string(arg.substr(0, colon)), days);
    return true;
}

/** The portfolios' contracts from IB into the database, on the gateway's own connection before
 * the engine opens it. False (with the reason printed) if any fails. */
auto sync(const std::vector<std::pair<std::string, int>>& portfolios) -> bool {
    try {
        core::LogEngine log;
        adapters::DatabaseEngine db(&log);
        const std::unique_ptr<adapters::Gateway> gateway = core::live::make_gateway(&log);
        gateway->connect();
        for (const auto& [portfolio, max_dte] : portfolios) {
            db.save_contracts(gateway->sync_contracts(portfolio, max_dte).get());
        }
        gateway->disconnect();
        db.close();
        // IB frees the client id a moment after the socket closes; the engine reuses it.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return true;
    } catch (const std::exception& e) {
        std::println(stderr, "Contract sync failed: {}", e.what());
        return false;
    }
}

/** Serves the gRPC EngineService until SIGINT / SIGTERM, then closes the engine. Returns 1 when a
 * sync fails (the engine does not start on yesterday's contracts). */
auto run_live(const LiveOptions& options) -> int {
    const std::string& listen_address = options.listen_address;
    // SIGINT / SIGTERM end the run: they are blocked here, before any thread starts (so in every
    // thread), and taken by the stopper thread, started first. Once the server runs it shuts it
    // down, so the engine is closed properly instead of the process being killed mid-write. Before
    // that (the contract sync, the engine's restore: nothing trades yet, and SQLite keeps its file
    // whole) the process exits at once, instead of a stop waiting for the sync to finish.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    // Static: the stopper outlives this function when it returns early (detached).
    static std::mutex serving_mutex;
    static grpc::Server* serving = nullptr;
    std::thread stopper([stop_signals] {
        int signal = 0;
        sigwait(&stop_signals, &signal);
        const std::lock_guard lock(serving_mutex);
        if (serving == nullptr) {
            std::println("Signal {}: stopping before the engine serves", signal);
            std::fflush(stdout);
            std::_Exit(128 + signal);
        }
        std::println("Signal {}: stopping", signal);
        std::fflush(stdout);
        // A deadline: the streams the UI keeps open (logs, strategy updates) end only once
        // cancelled, and without one Shutdown waits for them for ever.
        serving->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    });

    if (!options.syncs.empty() && !sync(options.syncs)) {
        stopper.detach(); // it waits for a signal that will not come: the process ends
        return 1;
    }
    core::live::MainEngine main_engine;

    // The gRPC service: the control plane reaches the engine through it only
    entry::GrpcLiveEngineService service(main_engine);

    // Start gRPC server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::println(stderr, "Failed to start gRPC server on {}", listen_address);
        stopper.detach();
        return 1;
    }
    {
        const std::lock_guard lock(serving_mutex);
        serving = server.get();
    }

    std::println("Live gRPC engine listening on {}", listen_address);
    std::fflush(stdout);
    if (options.resume) {
        main_engine.resume(); // served meanwhile: the UI sees it come back
    }
    // Block until a stop signal
    server->Wait();
    stopper.join();

    // Close engine before exit
    try {
        main_engine.disconnect();
        main_engine.close();
    } catch (...) {
        // Fail quietly, avoid exception from main
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    LiveOptions options;
    const char* env_address = std::getenv("LIVE_GRPC_ADDRESS");
    std::string& address = options.listen_address;
    if (env_address != nullptr && *env_address != '\0') {
        address = env_address;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(stdout);
            return 0;
        }
        if (arg == "--listen" && i + 1 < argc) {
            address = argv[++i];
            continue;
        }
        if (arg == "--sync" && i + 1 < argc && parse_sync(argv[i + 1], options)) {
            ++i;
            continue;
        }
        if (arg == "--resume") {
            options.resume = true;
            continue;
        }
        std::println(stderr, "entry_live: unknown argument '{}'\n", arg);
        print_usage(stderr);
        return 2;
    }
    return run_live(options);
}
