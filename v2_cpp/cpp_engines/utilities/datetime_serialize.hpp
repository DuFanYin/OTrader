#pragma once

/** ISO 8601 datetime serialization for ZMQ/Protobuf. */

#include "object.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace utilities {

/** Convert DateTime to ISO 8601 string (UTC). */
inline std::string datetime_to_iso_string(const DateTime& dt) {
    auto t = std::chrono::system_clock::to_time_t(dt);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

/** Parse ISO 8601 string to DateTime. Returns nullopt on parse error. */
inline std::optional<DateTime> datetime_from_iso_string(const std::string& s) {
    if (s.empty())
        return std::nullopt;
    std::tm t{};
    int y, M, d, h, m, sec;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &M, &d, &h, &m, &sec) >= 6) {
        t.tm_year = y - 1900;
        t.tm_mon = M - 1;
        t.tm_mday = d;
        t.tm_hour = h;
        t.tm_min = m;
        t.tm_sec = sec;
        return std::chrono::system_clock::from_time_t(std::mktime(&t));
    }
    return std::nullopt;
}

} // namespace utilities
