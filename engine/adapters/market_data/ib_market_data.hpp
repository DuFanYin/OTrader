#pragma once

/** Live IB quotes on their own connection (IB_CLIENT_ID + 1), within IB_MARKET_DATA_LINES. */

#include "core/market_data/market_data_engine.hpp"
#include <chrono>
#include <memory>
#include <string>

namespace adapters {

class IbMarketDataSession; // the connection, in ib_market_data.cpp

class IbMarketData final : public core::MarketData {
  public:
    IbMarketData();
    ~IbMarketData() override;

    void start(core::MarketDataEngine& engine) override;
    void stop() override;
    /** IB's daily bars of the underlying up to the day (regular hours): the day's close. */
    void ask_close(const std::string& underlying, std::chrono::year_month_day day) override;
    /** IB's minute bars of the underlying on the day (regular hours; midpoints, an index's
     * trades): the first's close, the highest high and lowest low. */
    void ask_day_range(const std::string& underlying, std::chrono::year_month_day day) override;

  private:
    std::unique_ptr<IbMarketDataSession> session_;
};

} // namespace adapters
