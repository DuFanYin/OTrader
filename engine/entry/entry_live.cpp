/**
 * entry_live: the live engine in one process, controlled only over gRPC. The gateway is IbGateway
 * or no broker depending on BUILD_GATEWAY, so this builds with or without IBJts.
 */

#include "assembly/live.hpp"
#include "entry/engine_grpc.hpp"
#include "strategy/strategy_registry.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/** Loopback only: the gRPC service has no authentication and can place orders. */
constexpr const char* kDefaultListenAddress = "localhost:50051";
constexpr int kDefaultSyncDays = 7;

struct LiveOptions {
    std::string listen_address = kDefaultListenAddress;
    /** The portfolios whose contracts are synced from IB each trading day (portfolio, max days
     * to expiry): the day the engine starts, then before each open. */
    std::vector<live::Synced> syncs;
    /** Start the strategies that were running, once the day's contracts are in. */
    bool resume = false;
};

void print_usage(std::FILE* out) {
    std::print(out,
               "Usage: entry_live [--listen ADDRESS] [--sync PORTFOLIO[:DAYS]]... [--resume]\n"
               "                  [--strategies DIR]...\n"
               "  Runs the live engine: the gRPC EngineService over core::Main (market data\n"
               "  and gateway in-process). It starts where it stopped: its strategies, inited if\n"
               "  they were, their holdings rebooked from the database. Once serving it connects\n"
               "  to IB, rolls in the day's contracts, starts the market data (and with --resume\n"
               "  the strategies that were running), and runs from day to day. Stops on SIGINT /\n"
               "  SIGTERM.\n"
               "\n"
               "  --listen ADDRESS  gRPC address (default: $LIVE_GRPC_ADDRESS, else {}).\n"
               "                    The service is unauthenticated and can fill: listen on\n"
               "                    another interface only behind your own access control.\n"
               "  --sync PORTFOLIO[:DAYS]\n"
               "                    the portfolio's contracts from IB, its options expiring\n"
               "                    within DAYS (default {}), synced and rolled in when the\n"
               "                    engine starts and each trading day at 08:45 New York (a\n"
               "                    failed sync is retried each minute); repeat for each one\n"
               "  --resume          start the strategies that were running, once the day's\n"
               "                    contracts are in (positions are checked against IB)\n"
               "  --strategies DIR  the strategy libraries there too, beside the system's\n"
               "                    (strategies/ by the program): those installed for the\n"
               "                    account it fills, strategies/paper or strategies/live\n"
               "  -h, --help        this help\n"
               "\n"
               "  It runs from day to day:\n"
               "    entry_live --sync SPY --sync SPXW --resume\n"
               "\n"
               "Environment (scripts/dev.sh loads .env): OPTRA_DB (default\n"
               "  data/optra.db), IB_HOST, IB_PORT, IB_CLIENT_ID (orders; quotes connect as\n"
               "  IB_CLIENT_ID + 1), IB_ACCOUNT (required: the one account traded, a paper one,\n"
               "  DU..., unless root names a live one in /etc/optra/live-account; IB's login\n"
               "  must manage it, or nothing connects), IB_MARKET_DATA_LINES (default 100),\n"
               "  IB_MARKET_DATA_TYPE (1 live, 2 frozen, 3 delayed, 4 delayed frozen),\n"
               "  OPTRA_BROKER_CHECK=warn (strategies start even when the positions disagree)\n",
               kDefaultListenAddress, kDefaultSyncDays);
}

/** PORTFOLIO[:DAYS]; none when DAYS is not a number of days. */
auto parse_sync(std::string_view arg) -> std::optional<live::Synced> {
    const auto colon = arg.find(':');
    int days = kDefaultSyncDays;
    if (colon != std::string_view::npos) {
        const std::string_view text = arg.substr(colon + 1);
        if (std::from_chars(text.data(), text.data() + text.size(), days).ec != std::errc{} ||
            days < 0) {
            return std::nullopt;
        }
    }
    return live::Synced{std::string(arg.substr(0, colon)), days};
}

} // namespace

int main(int argc, char* argv[]) {
    // The system's strategies; those installed for the account it trades come with --strategies
    // (strategies/paper or strategies/live), never the research ones. Listing the classes loads
    // those built since.
    strategy::StrategyRegistry::load(strategy::StrategyRegistry::beside_executable());
    LiveOptions options;
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
        if (arg == "--sync" && i + 1 < argc) {
            if (std::optional<live::Synced> sync = parse_sync(argv[i + 1])) {
                options.syncs.push_back(std::move(*sync));
                ++i;
                continue;
            }
        }
        if (arg == "--resume") {
            options.resume = true;
            continue;
        }
        if (arg == "--strategies" && i + 1 < argc) {
            strategy::StrategyRegistry::load(argv[++i]);
            continue;
        }
        std::println(stderr, "entry_live: unknown argument '{}'\n", arg);
        print_usage(stderr);
        return 2;
    }
    return entry::serve([] { return live::make_main(); },
                        options.listen_address, std::move(options.syncs), options.resume);
}
