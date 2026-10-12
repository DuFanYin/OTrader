#pragma once

/**
 * IbFeed: live quotes from IB Gateway / TWS, on a connection of its own as IB_CLIENT_ID + 1 (the
 * orders' connection is the gateway's). Streams what the quote book plans within the account's
 * IB_MARKET_DATA_LINES, of IB_MARKET_DATA_TYPE (1 live, 3 delayed).
 */

#include "../marketdata/market_data_feed.hpp"
#include <memory>

namespace adapters {

class IbFeedSession; // the connection, in feed_ib.cpp

class IbFeed final : public MarketDataFeed {
  public:
    IbFeed();
    ~IbFeed() override;

    void start(QuoteBook& book, const utilities::BaseEngine& log) override;
    void stop() override;

  private:
    std::unique_ptr<IbFeedSession> session_;
};

} // namespace adapters
