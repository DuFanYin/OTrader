/**
 * entry_system.cpp
 *
 * Unified entry for the live system. This TU is a thin dispatcher only — each mode lives in
 * its own translation unit under entry/ so the binary builds independently of any gateway
 * backend:
 *
 *   entry_system --mode=gateway   # ZMQ REP+PUB + IbGateway   (entry/run_gateway.cpp, IB-only)
 *   entry_system --mode=market    # ZMQ REP+PUB + MarketData  (entry/run_market.cpp)
 *   entry_system --mode=live      # gRPC + MainEngine runtime  (entry/run_live.cpp, default)
 *   entry_system --mode=all       # gateway + market subprocs, then live (entry/run_gateway.cpp)
 *
 * The gateway/all modes exist only when built with BUILD_GATEWAY=ON (OTRADER_WITH_IB); without
 * it this file never references the IB gateway and rejects those modes at runtime.
 */

#include "entry/entry_modes.hpp"

#include <cstdio>
#include <string>
#include <string_view>

int main(int argc, char* argv[]) {
    std::string mode = "live"; // default: live gRPC runtime
    if (argc >= 2) {
        std::string_view arg{argv[1]};
        if (arg.rfind("--mode=", 0) == 0) {
            mode = std::string(arg.substr(7));
        }
    }

    if (mode == "live")
        return entry::run_live();
    if (mode == "market")
        return entry::run_market();
    if (mode == "gateway" || mode == "all") {
#ifdef OTRADER_WITH_IB
        return mode == "gateway" ? entry::run_gateway() : entry::run_system_all();
#else
        std::fprintf(stderr,
                     "entry_system: built without IB gateway support (BUILD_GATEWAY=OFF). "
                     "Rebuild with -DBUILD_GATEWAY=ON to use --mode=%s.\n",
                     mode.c_str());
        return 1;
#endif
    }

    std::fprintf(stderr,
                 "Usage: entry_system --mode=gateway|market|live|all\n"
                 "  gateway : run ZMQ + IB gateway process (needs BUILD_GATEWAY=ON)\n"
                 "  market  : run market-data provider process\n"
                 "  live    : run gRPC + MainEngine runtime (default)\n"
                 "  all     : start gateway + market as subprocesses, then live in foreground\n");
    return 1;
}
