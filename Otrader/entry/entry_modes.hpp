#pragma once

/**
 * Entry point for entry_system: the live runtime (gRPC + single-process MainEngine).
 * Market-data and gateway are now in-process inside MainEngine, so there is a single mode.
 */

namespace entry {

int run_live();

} // namespace entry
