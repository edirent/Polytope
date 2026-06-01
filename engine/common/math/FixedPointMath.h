#pragma once

#include <cstdint>
#include <limits>

namespace trading_engine::common::math {

[[nodiscard]] inline bool checked_add_i64(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) + static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max() ||
        value < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] inline bool checked_sub_i64(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) - static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max() ||
        value < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] inline bool checked_mul_i64(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) * static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max() ||
        value < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] inline std::int64_t saturating_add_i64(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    std::int64_t out = 0;
    if (checked_add_i64(lhs, rhs, &out)) {
        return out;
    }
    return rhs >= 0 ? std::numeric_limits<std::int64_t>::max()
                    : std::numeric_limits<std::int64_t>::min();
}

[[nodiscard]] inline std::int64_t saturating_mul_i64(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    std::int64_t out = 0;
    if (checked_mul_i64(lhs, rhs, &out)) {
        return out;
    }
    const bool positive = (lhs >= 0 && rhs >= 0) || (lhs < 0 && rhs < 0);
    return positive ? std::numeric_limits<std::int64_t>::max()
                    : std::numeric_limits<std::int64_t>::min();
}

[[nodiscard]] inline std::int64_t ceil_div_positive(
    std::int64_t numerator,
    std::int64_t denominator
) noexcept {
    if (denominator <= 0) {
        return 0;
    }
    const auto value =
        (static_cast<__int128>(numerator) + denominator - 1) /
        static_cast<__int128>(denominator);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] inline std::int64_t ratio_bps(
    std::int64_t numerator,
    std::int64_t denominator
) noexcept {
    if (denominator == 0) {
        if (numerator > 0) {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (numerator < 0) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return 0;
    }

    const auto value =
        static_cast<__int128>(numerator) * 10'000 /
        static_cast<__int128>(denominator);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value < std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

}  // namespace trading_engine::common::math
