#pragma once

/** A backtest on a replay of its own, as a test drives one: a day rolled in, then its bars applied
 * and run one by one, as run_backtests does for the backtests of a thread. */

#include "adapters/market_data/historical_data.hpp"
#include "assembly/backtest.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace test {

struct OwnReplay {
    backtest::Replay replay;
};

class LoneBacktest : private OwnReplay, public backtest::Backtest {
  public:
    LoneBacktest() : Backtest(replay) {}

    /** The day loaded, priced with the defaults, and used. */
    adapters::HistoricalData* load_backtest_data(const std::string& parquet_path) {
        return use_day(adapters::load_day(parquet_path, adapters::HistoricalData::kRiskFreeRate,
                                          adapters::HistoricalData::kIvPriceMode));
    }
    /** The day rolled into the replay, then used. */
    adapters::HistoricalData* use_day(const adapters::LoadedDay& day) {
        day_ = day;
        const std::vector<std::string> dropped =
            day->has_data() ? replay.roll(*day) : std::vector<std::string>{};
        return Backtest::use_day(day, dropped);
    }
    /** The bar applied, then run. */
    void run_bar(std::size_t i) {
        replay.apply(day_, i);
        Backtest::run_bar(i);
    }
    /** The day, every bar. */
    backtest::DailyResult run() {
        if (begin_day()) {
            for (std::size_t i = 0; i < day_->minutes(); ++i) {
                run_bar(i);
            }
        }
        return end_day();
    }

  private:
    adapters::LoadedDay day_;
};

} // namespace test
