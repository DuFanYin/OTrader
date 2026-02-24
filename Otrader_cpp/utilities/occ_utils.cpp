#include "occ_utils.hpp"
#include "constant.hpp"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace backtest {

namespace {
using namespace utilities;
}

std::tuple<std::optional<Timestamp>, std::optional<double>, std::optional<OptionType>>
parse_occ_symbol(const std::string& symbol) {
    if (symbol.size() < 15u) return {std::nullopt, std::nullopt, std::nullopt};
    try {
        int yy = std::stoi(symbol.substr(0, 2));
        int mm = std::stoi(symbol.substr(2, 2));
        int dd = std::stoi(symbol.substr(4, 2));
        int year = (yy < 80) ? (2000 + yy) : (1900 + yy);
        // Expiry 16:00 ET = 21:00 UTC; build UTC tm and convert to time_t without touching TZ
        std::tm tm_utc{};
        tm_utc.tm_year = year - 1900;
        tm_utc.tm_mon = mm - 1;
        tm_utc.tm_mday = dd;
        tm_utc.tm_hour = 21;
        tm_utc.tm_min = 0;
        tm_utc.tm_sec = 0;
        tm_utc.tm_isdst = 0;
        std::time_t t;
#ifdef _WIN32
        t = _mkgmtime(&tm_utc);
#else
        t = timegm(&tm_utc);
#endif
        if (t == -1) return {std::nullopt, std::nullopt, std::nullopt};
        Timestamp expiry = std::chrono::system_clock::from_time_t(t);

        char cp = static_cast<char>(std::toupper(static_cast<unsigned char>(symbol[6])));
        if (cp != 'C' && cp != 'P') return {std::nullopt, std::nullopt, std::nullopt};
        double strike = std::stoi(symbol.substr(7, 8)) / 1000.0;
        OptionType opt_type = (cp == 'C') ? OptionType::CALL : OptionType::PUT;
        return {expiry, strike, opt_type};
    } catch (...) {
        return {std::nullopt, std::nullopt, std::nullopt};
    }
}

std::string infer_underlying_from_filename(const std::string& filename) {
    // 支持两类命名：
    // 1) 旧格式：backtest_SPX_20250220.parquet  -> SPX
    // 2) 新格式：data/SPXW/SPXW-2025-08/20250801.parquet -> SPXW（取上层目录名，去掉 -2025-08 等后缀）

    // 先取文件名部分
    std::string name = filename;
    auto pos = name.find_last_of("/\\");
    if (pos != std::string::npos) name = name.substr(pos + 1);
    pos = name.find('.');
    if (pos != std::string::npos) name = name.substr(0, pos);

    // 旧 backtest_ 前缀格式
    if (name.size() >= 9 && name.substr(0, 9) == "backtest_") {
        pos = name.find('_', 9);
        if (pos == std::string::npos) return "";
        std::string underlying = name.substr(9, pos - 9);
        for (char& c : underlying) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return underlying;
    }

    // 新 SPXW 目录结构：从路径中抽取上级目录名
    // e.g. data/SPXW/SPXW-2025-08/20250801.parquet
    // -> parent_dir = "SPXW-2025-08" -> underlying = "SPXW"
    std::string path = filename;
    // 去掉最后一级文件名
    auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return "";
    path = path.substr(0, slash);
    // 取父目录名
    auto slash2 = path.find_last_of("/\\");
    std::string parent = (slash2 == std::string::npos) ? path : path.substr(slash2 + 1);
    if (parent.empty()) return "";
    // 去掉类似 "-2025-08" 的后缀
    auto dash = parent.find('-');
    std::string underlying = (dash == std::string::npos) ? parent : parent.substr(0, dash);
    for (char& c : underlying) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return underlying;
}

std::string format_expiry_yyyymmdd(Timestamp expiry) {
    auto t = std::chrono::system_clock::to_time_t(expiry);
    std::tm* tm = std::gmtime(&t);
    if (!tm) return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d",
                  tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return buf;
}

}  // namespace backtest
