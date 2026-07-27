/**
 * entry_system.cpp
 *
 * Entry for the live system: a single process running the gRPC service over MainEngine, which
 * hosts the market-data and gateway engines in-process (see runtime/live/engine_main.*). The old
 * market/gateway/all sub-processes and their ZMQ data plane are gone — everything is one process.
 */

#include "entry/entry_modes.hpp"

int main() { return entry::run_live(); }
