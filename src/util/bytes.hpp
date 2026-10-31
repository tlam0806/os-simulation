// Little-endian reads and writes of 16- and 32-bit values in a byte buffer.
// Callers check bounds first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace util {

inline uint16_t get16(std::span<const uint8_t> f, size_t at) {
    return static_cast<uint16_t>(f[at] | f[at + 1] << 8);
}

inline uint32_t get32(std::span<const uint8_t> f, size_t at) {
    return uint32_t{f[at]} | uint32_t{f[at + 1]} << 8 | uint32_t{f[at + 2]} << 16 |
           uint32_t{f[at + 3]} << 24;
}

inline void put16(std::span<uint8_t> f, size_t at, uint16_t value) {
    f[at] = static_cast<uint8_t>(value);
    f[at + 1] = static_cast<uint8_t>(value >> 8);
}

inline void put32(std::span<uint8_t> f, size_t at, uint32_t value) {
    f[at] = static_cast<uint8_t>(value);
    f[at + 1] = static_cast<uint8_t>(value >> 8);
    f[at + 2] = static_cast<uint8_t>(value >> 16);
    f[at + 3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace util
