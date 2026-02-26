#include "utility.hpp"
#include <sstream>
#include <string>

namespace utilities {

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

auto ceil_to(double value, double target) -> double {
    if (target <= 0) {
        return value;
    }
    return std::ceil(value / target) * target;
}

auto get_digits(double value) -> int {
    std::ostringstream os;
    os << value;
    std::string s = os.str();
    auto pos = s.find('.');
    if (pos != std::string::npos) {
        return static_cast<int>(s.size() - pos - 1);
    }
    auto e = s.find("e-");
    if (e != std::string::npos) {
        return std::stoi(s.substr(e + 2));
    }
    return 0;
}

auto calculate_days_to_expiry(std::chrono::system_clock::time_point option_expiry) -> int {
    auto now = std::chrono::system_clock::now();
    if (option_expiry <= now) {
        return 0;
    }
    auto diff = std::chrono::duration_cast<std::chrono::hours>(option_expiry - now).count();
    return static_cast<int>(diff / 24);
}

auto calculate_days_to_expiry(std::chrono::system_clock::time_point* option_expiry) -> int {
    if (option_expiry == nullptr) {
        return 0;
    }
    return calculate_days_to_expiry(*option_expiry);
}

} // namespace utilities
