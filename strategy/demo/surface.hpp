#pragma once

/**
 * SurfaceStrategy: trades nothing; measures the day's volatility surface once, `before_close`
 * minutes before the session's close (15:45 on a full day, 12:45 on a half day). Each
 * day it writes, IVs on calendar time (365 days), at constant maturities from now, the two chains
 * around each interpolated (ATM: total variance linear in years; the rest linear in years):
 * atm_iv_7d, atm_iv_30d, atm_iv_90d (each chain's ATM IV on its smile), rr25_30d (a call's IV at
 * delta 0.25 less a put's at -0.25), bf25_30d (their mean less the ATM IV), term_slope
 * (atm_iv_90d less atm_iv_30d) and spot (the underlying's mid then). A value it cannot work out
 * that day is not written. Raw SVI parameters are not written: on a near-parabolic smile they jump
 * between equivalent fits from day to day.
 */

#include "strategy.hpp"

#include <string>
#include <vector>

namespace strategy {

class SurfaceStrategy final : public Strategy {
  public:
    struct Setting {
        Param<int> before_close{"before_close", 15, 0, 389};
    };
    using Strategy::Strategy;

    void on_day() override;
    void on_timer() override;

  private:
    void measure();

    const Setting setting_ = read_setting<Setting>();
    std::vector<std::string> chains_; // around each maturity
    bool measured_ = false;
};

} // namespace strategy
