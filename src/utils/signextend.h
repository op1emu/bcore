#pragma once

#include <cstdint>

// Sign-extend an N-bit value to int32_t.
// Example: signextend<4>(0x8) == -8, signextend<10>(0x3ff) == -1
template <unsigned N>
inline int32_t signextend(uint32_t x) {
    static_assert(N > 0 && N <= 31, "N must be 1..31");
    constexpr uint32_t mask = 1u << (N - 1);
    return static_cast<int32_t>((x ^ mask) - mask);
}
