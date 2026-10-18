/**
 * entry_simulate: the unchanged live engine on a simulated market (the public demo): recorded
 * Parquet days replayed at wall-clock pace, orders filled at their quotes in a simulated account.
 */

#include "adapters/market_data/data_loader.hpp"
#include "adapters/gateway/simulated_gateway.hpp"
#include "adapters/market_data/simulated_market_data.hpp"
#include "assembly/live.hpp"
#include "entry/engine_grpc.hpp"
#include "strategy/strategy_registry.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/** Loopback only: the gRPC service has no authentication and can place orders. */
constexpr const char* kDefaultListenAddress = "localhost:50051";
/** After the last quote, the engine keeps serving this long, then stops. */
constexpr auto kAfterSession = std::chrono::minutes(15);
constexpr double kFeeRate = 1.2; // USD a contract, as the backtest's default

struct SimulateOptions {
    std::string listen_address = kDefaultListenAddress;
    /** Start the strategies that were running, once the days' contracts are in. */
    bool resume = false;
    std::vector<std::string> days;
};

void print_usage(std::FILE* out) {
    std::print(out,
               "Usage: entry_simulate [--listen ADDRESS] [--resume] [--strategies DIR]... DAY.parquet...\n"
               "  Runs the live engine on a simulated market, served like entry_live: the days\n"
               "  (the backtest's Parquet, under data/<PORTFOLIO>/; one per portfolio) replayed\n"
               "  from their first minute at the wall clock's pace, orders filled at their quotes.\n"
               "  The contracts come from the files; the account is kept in $OPTRA_DB.account.json.\n"
               "  Stops on SIGINT / SIGTERM, or 15 minutes after the last quote.\n"
               "\n"
               "  --listen ADDRESS  gRPC address (default {}; env LIVE_GRPC_ADDRESS)\n"
               "  --resume          start the strategies that were running, once the days'\n"
               "                    contracts are in (the market data starts either way)\n"
               "  --strategies DIR  the strategy libraries there too, beside the system's\n"
               "  -h, --help        this help\n",
               kDefaultListenAddress);
}

auto account_file() -> std::string {
    const char* db = std::getenv("OPTRA_DB");
    return std::string(db != nullptr && *db != '\0' ? db : "data/optra.db") + ".account.json";
}

/** Serves the engine (entry::serve) until SIGINT / SIGTERM or kAfterSession past the session's
 * end. The days' contracts come in as a broker's would (SimulatedGateway::sync_contracts). */
auto run(const SimulateOptions& options) -> int {
    std::pair<model::DateTime, model::DateTime> session;
    try {
        session = adapters::time_range_of_files(options.days);
    } catch (const std::exception& e) {
        std::println(stderr, "The days: {}", e.what());
        return 1;
    }
    // Starts at the days' first quote at wall-clock pace; venue, replay and engine share it.
    const auto offset = std::chrono::system_clock::now() - session.first;
    const live::Clock clock = [offset] {
        return std::chrono::system_clock::now() - offset;
    };
    adapters::SimulatedGateway* venue = nullptr;
    core::Main* engine = nullptr; // set before the replay starts (with the run)
    const auto make = [&]() -> std::unique_ptr<core::Main> {
        // After each minute's quotes: the orders they fill, on the event thread like every booking.
        auto market_data =
            std::make_unique<adapters::SimulatedMarketData>(options.days, clock, [&venue, &engine] {
                try {
                    engine->event_loop()->on_event_thread([&venue] { venue->on_minute(); });
                } catch (const std::exception&) { // the engine is stopping
                }
            });
        auto made = live::make_main(
            [&](utilities::Logger*, const model::Portfolios& portfolios) {
                auto gateway = std::make_unique<adapters::SimulatedGateway>(
                    portfolios, options.days, account_file(), clock, kFeeRate);
                venue = gateway.get();
                return gateway;
            },
            std::move(market_data), clock);
        engine = made.get();
        engine->write_log("Simulated market: no broker; quotes replayed, orders filled at them",
                          utilities::WARNING);
        return made;
    };
    std::vector<live::Synced> portfolios;
    for (const std::string& portfolio : adapters::portfolios_of_files(options.days)) {
        portfolios.emplace_back(portfolio, 0); // the days' contracts, whatever their expiry
    }
    const auto lasts = std::chrono::ceil<std::chrono::seconds>(session.second - session.first +
                                                               kAfterSession);
    return entry::serve(make, options.listen_address, std::move(portfolios), options.resume,
                        lasts);
}

} // namespace

int main(int argc, char* argv[]) {
    // The system's strategies; those installed for the account it trades come with --strategies
    // (strategies/paper or strategies/live), never the research ones. Listing the classes loads
    // those built since.
    strategy::StrategyRegistry::load(strategy::StrategyRegistry::beside_executable());
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
        if (arg == "--strategies" && i + 1 < argc) {
            strategy::StrategyRegistry::load(argv[++i]);
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
