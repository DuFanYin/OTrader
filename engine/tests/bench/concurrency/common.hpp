#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>

// Peak resident set size of this process in MiB (Linux reports ru_maxrss in KiB).
inline double peak_rss_mib() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_maxrss / 1024.0;
}

inline double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// --name value
inline std::string arg(int argc, char** argv, const char* name, const char* def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}
