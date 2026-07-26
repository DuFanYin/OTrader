#pragma once

/**
 * Per-mode entry points for entry_system, each defined in its own translation unit so the
 * engine builds independently of any gateway backend:
 *
 *   - run_live()    / run_market()       — no gateway/IB dependency (entry/run_live.cpp,
 *                                          entry/run_market.cpp)
 *   - run_gateway() / run_system_all()   — depend on the IB gateway (entry/run_gateway.cpp);
 *                                          compiled only when BUILD_GATEWAY=ON (which defines
 *                                          OTRADER_WITH_IB). entry_system.cpp rejects the
 *                                          gateway/all modes at runtime when not built.
 */

namespace entry {

int run_live();
int run_market();

#ifdef OTRADER_WITH_IB
int run_gateway();
int run_system_all();
#endif

} // namespace entry
