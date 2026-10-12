#include "utility.hpp"
#include <algorithm>
#include <format>
#include <string>

namespace model {

namespace {

auto new_york() -> const std::chrono::time_zone* {
    static const std::chrono::time_zone* const zone = std::chrono::locate_zone("America/New_York");
    return zone;
}

} // namespace

auto round_to(double value, double target) -> double {
    if (target <= 0) {
        return value;
    }
    return std::round(value / target) * target;
}

auto floor_to(double value, double target) -> double {
    if (target <= 0) {
        return value;
    }
    return std::floor(value / target) * target;
}

auto years_to_expiry(std::chrono::system_clock::time_point now,
                     std::chrono::system_clock::time_point expiry) -> double {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(expiry - now).count();
    if (secs <= 0) {
        return 0.0;
    }
    return std::max(1e-6, static_cast<double>(secs) / (365.25 * 24.0 * 3600.0));
}

auto option_expiry_time(std::chrono::year_month_day date) -> std::chrono::system_clock::time_point {
    using namespace std::chrono;
    return zoned_time{new_york(), local_days{date} + 16h}.get_sys_time();
}

auto new_york_date(std::chrono::system_clock::time_point t) -> std::chrono::year_month_day {
    using namespace std::chrono;
    return year_month_day{floor<days>(new_york()->to_local(t))};
}

auto minutes_since_open(std::chrono::system_clock::time_point t) -> int {
    using namespace std::chrono;
    const auto local = new_york()->to_local(t);
    const auto open = floor<days>(local) + 9h + 30min;
    return static_cast<int>(floor<minutes>(local - open).count());
}

auto BarGate::next(std::chrono::system_clock::time_point event_time) -> bool {
    const auto bar = std::chrono::floor<std::chrono::minutes>(event_time);
    if (bar <= last_bar_) {
        return false;
    }
    last_bar_ = bar;
    return true;
}

} // namespace model
