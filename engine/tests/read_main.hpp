#pragma once

/** What tests read of a running Main, through Main::read. */

#include "core/main.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace optra_tests {

/** Its strategies' records, in the order added. */
inline auto strategies(core::Main& main) -> std::vector<model::StrategyRecord> {
    return main.read([](const core::Main& m) {
        return std::vector<model::StrategyRecord>(std::from_range,
                                                  m.strategy_engine()->strategies());
    });
}

/** Each strategy's holding, by name. */
inline auto holdings(core::Main& main) -> std::unordered_map<std::string, model::StrategyHolding> {
    return main.read([](const core::Main& m) {
        std::unordered_map<std::string, model::StrategyHolding> out;
        for (const model::StrategyRecord& r : m.strategy_engine()->strategies()) {
            if (const model::StrategyHolding* h = m.bookkeeping_engine()->find_holding(r.name)) {
                out.emplace(r.name, *h);
            }
        }
        return out;
    });
}

/** The strategy's record (event thread), or one of its name alone when it is not hosted: whom an
 * order sent through Main::send_order is for. */
inline auto record_of(const core::Main& main, const std::string& name) -> model::StrategyRecord {
    const model::StrategyRecord* hosted = main.strategy_engine()->find_record(name);
    return hosted != nullptr ? *hosted : model::StrategyRecord{.name = name};
}

} // namespace optra_tests
