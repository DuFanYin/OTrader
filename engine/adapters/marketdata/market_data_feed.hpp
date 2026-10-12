#pragma once

#include "quote_book.hpp"

namespace utilities {
struct BaseEngine;
} // namespace utilities

namespace adapters {

/** A source of live quotes (IB; Databento, ...). Once started it streams, on a thread of its own,
 * what the book plans (QuoteBook::instruments) into the book, and has the book publish. */
class MarketDataFeed {
  public:
    virtual ~MarketDataFeed() = default;

    /** Connect and start streaming into book, logging through log; throws when the source cannot
     * be reached. */
    virtual void start(QuoteBook& book, const utilities::BaseEngine& log) = 0;
    /** Stop streaming and disconnect; nothing touches the book after it returns. */
    virtual void stop() = 0;
};

} // namespace adapters
