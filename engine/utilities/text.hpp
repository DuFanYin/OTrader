#pragma once

/** Text helpers. */

#include <string>

namespace utilities {

/** The items, ", " between them. */
template <typename Range> std::string join(const Range& items) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) {
            out += ", ";
        }
        out += item;
    }
    return out;
}

} // namespace utilities
