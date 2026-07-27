#pragma once

/**
 * LiveGateway: the gateway type the live MainEngine holds in-process. Selected at compile
 * time by BUILD_GATEWAY (which defines OTRADER_WITH_IB and links IBJts):
 *   - OTRADER_WITH_IB defined  → IbGateway (real Interactive Brokers TWS gateway)
 *   - otherwise                → NullGateway (logs + rejects orders, zero IB dependency)
 *
 * Deliberately a plain `using` alias, not a virtual interface or dlopen: the gateway is
 * expected to be rewritten directly (e.g. a low-latency exchange gateway) rather than
 * swapped at runtime, so no abstraction layer is carried that a rewrite would have to remove.
 * IbGateway and NullGateway expose the identical public surface, so this alias is drop-in.
 */

#ifdef OTRADER_WITH_IB
#include "engine_gateway_ib.hpp"
namespace engines {
using LiveGateway = IbGateway;
} // namespace engines
#else
#include "engine_gateway_null.hpp"
namespace engines {
using LiveGateway = NullGateway;
} // namespace engines
#endif
