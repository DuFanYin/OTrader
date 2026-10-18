#pragma once

/** Fnv: FNV-1a, 64 bits, over values' bytes: a key that is the same wherever the same values are
 * hashed in the same order (native byte order). */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace utilities {

class Fnv {
  public:
    void add(std::span<const std::byte> bytes) {
        for (const std::byte b : bytes) {
            hash_ = (hash_ ^ static_cast<std::uint64_t>(b)) * 0x100000001b3ULL;
        }
    }
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void add(const T& value) {
        add(std::as_bytes(std::span(&value, 1)));
    }
    void add(std::string_view text) {
        add<std::uint64_t>(text.size());
        add(std::as_bytes(std::span(text)));
    }
    void add(std::span<const double> values) {
        add<std::uint64_t>(values.size());
        add(std::as_bytes(values));
    }
    [[nodiscard]] std::uint64_t value() const { return hash_; }

  private:
    std::uint64_t hash_ = 0xcbf29ce484222325ULL;
};

} // namespace utilities
