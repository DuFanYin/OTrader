/**
 * In a test or a benchmark that runs strategies (optra_strategy_host with LOAD): the build's strategy
 * libraries, loaded as the program starts, before any test runs.
 */

#include "strategy/strategy_registry.hpp"

namespace {

[[maybe_unused]] const bool loaded = [] {
    strategy::StrategyRegistry::load(OPTRA_STRATEGY_LIBRARIES);
    return true;
}();

} // namespace
