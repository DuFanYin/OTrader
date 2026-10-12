#pragma once

/**
 * RuntimeAPI: what the core engines ask of the engine around them (core::Engine, which
 * implements it), the one way they reach past the core: the venue (orders out), the live market
 * data, the storage. The core engines reach each
 * other directly.
 */

#include "../model/object.hpp"

#include <span>
#include <string>

namespace core {

class RuntimeAPI {
  public:
    /** Through the venue, tracked as the strategy's: the order's id, empty when rejected. */
    virtual std::string send_order(const std::string& strategy_name,
                                   const model::OrderRequest& req) = 0;
    /** Through the venue; the order is no longer tracked as working. */
    virtual void cancel_order(const model::CancelRequest& req) = 0;
    /** Live quotes for the chains a strategy trades (none in a backtest, which replays them all). */
    virtual void subscribe_chains(const std::string& strategy_name,
                                  std::span<const std::string> chains) = 0;
    virtual void unsubscribe_chains(const std::string& strategy_name) = 0;
    /** A name some strategy had before (live: its orders and trades are kept under it in the
     * database): never given to another. */
    virtual bool is_strategy_name_used(const std::string& strategy_name) = 0;

  protected:
    ~RuntimeAPI() = default;
};

} // namespace core
