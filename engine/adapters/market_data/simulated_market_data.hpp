#pragma once

/** The simulated market's quotes: recorded Parquet days streamed a minute at a time. */

#include "core/market_data/market_data_engine.hpp"
#include "model/object.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace adapters {

class SimulatedMarketData final : public core::MarketData {
  public:
    using Clock = std::function<model::DateTime()>;

    /** after_minute: called on the source's thread once each batch of minutes is published. */
    SimulatedMarketData(std::vector<std::string> files, Clock clock,
                        std::function<void()> after_minute);
    ~SimulatedMarketData() override;

    void start(core::MarketDataEngine& engine) override;
    void stop() override;
    /** Answered from the day's file beside the files it streams (<PORTFOLIO>-YYYY-MM/YYYYMMDD):
     * the underlying's last mid of the day; unanswered when there is none. */
    void ask_close(const std::string& underlying, std::chrono::year_month_day day) override;
    /** From the same file: the underlying's mid at its first minute, the highest and lowest of
     * its minutes until the clock's time (all of them when the clock gives none). */
    void ask_day_range(const std::string& underlying, std::chrono::year_month_day day) override;

  private:
    /** A close asked for, or a day's range. */
    struct Asked {
        std::string underlying;
        std::chrono::year_month_day day;
        bool range = false;
    };

    /** One minute of one portfolio: its underlying's quote and its options'. */
    struct Minute {
        model::DateTime time{};
        std::string underlying;
        double underlying_bid = 0.0;
        double underlying_ask = 0.0;
        std::vector<std::string> options; // the platform's names
        std::vector<double> bids;
        std::vector<double> asks;
        std::vector<double> bid_sizes;
        std::vector<double> ask_sizes;
    };

    void load();
    void run(core::MarketDataEngine& engine);
    /** What was asked for, answered (the source's thread). */
    void answer(core::MarketDataEngine& engine, const std::vector<Asked>& asked);

    std::vector<std::string> files_;
    Clock clock_;
    std::function<void()> after_minute_;
    std::vector<Minute> minutes_; // every portfolio's, in time order
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::vector<Asked> asked_; // to answer
    std::thread thread_;
};

} // namespace adapters
