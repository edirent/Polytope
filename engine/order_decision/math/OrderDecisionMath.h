#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::order_decision {

inline constexpr std::uint64_t kOrderDecisionFnvOffset =
    14695981039346656037ULL;
inline constexpr std::uint64_t kOrderDecisionFnvPrime = 1099511628211ULL;

inline void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & 0xffU;
        *hash *= kOrderDecisionFnvPrime;
    }
}

inline void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

inline void mix_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (const unsigned char ch : value) {
        *hash ^= ch;
        *hash *= kOrderDecisionFnvPrime;
    }
}

}  // namespace trading_engine::order_decision
