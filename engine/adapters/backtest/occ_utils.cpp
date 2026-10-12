#include "occ_utils.hpp"

namespace adapters {

auto infer_underlying_from_filename(const std::string& filename) -> std::string {
    // Filename formats: backtest_SPX_* or data/SPXW/SPXW-2025-08/*

    // Filename
    std::string name = filename;
    auto pos = name.find_last_of("/\\");
    if (pos != std::string::npos) {
        name = name.substr(pos + 1);
    }
    pos = name.find('.');
    if (pos != std::string::npos) {
        name = name.substr(0, pos);
    }

    // backtest_ prefix
    if (name.size() >= 9 && name.starts_with("backtest_")) {
        pos = name.find('_', 9);
        if (pos == std::string::npos) {
            return "";
        }
        std::string underlying = name.substr(9, pos - 9);
        for (char& c : underlying) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return underlying;
    }

    // SPXW dir: parent dir = underlying
    std::string path = filename;
    // Strip filename
    auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return "";
    }
    path = path.substr(0, slash);
    // Parent dir
    auto slash2 = path.find_last_of("/\\");
    std::string parent = (slash2 == std::string::npos) ? path : path.substr(slash2 + 1);
    if (parent.empty()) {
        return "";
    }
    // Strip suffix like "-2025-08"
    auto dash = parent.find('-');
    std::string underlying = (dash == std::string::npos) ? parent : parent.substr(0, dash);
    for (char& c : underlying) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return underlying;
}

} // namespace adapters
