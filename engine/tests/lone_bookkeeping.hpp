#pragma once

/** A BookkeepingEngine on portfolios of its own, nothing listed: an option it books takes the
 * multiplier of one not listed (model::Contract::kOptionMultiplierDefault). */

#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "model/portfolios.hpp"

namespace test {

struct OwnPortfolios {
    model::Portfolios portfolios;
};

class LoneBookkeeping : private OwnPortfolios, public core::BookkeepingEngine {
  public:
    explicit LoneBookkeeping(utilities::Logger* logger = nullptr)
        : BookkeepingEngine(logger, portfolios) {}
};

} // namespace test
