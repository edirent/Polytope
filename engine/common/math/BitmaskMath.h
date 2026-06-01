#pragma once

#include <cstdint>

namespace trading_engine::common::math {

[[nodiscard]] inline bool bitmask_has_conflict(
    std::uint64_t true_mask,
    std::uint64_t false_mask
) noexcept {
    return (true_mask & false_mask) != 0;
}

[[nodiscard]] inline bool bitmask_satisfies_required(
    std::uint64_t state_true_mask,
    std::uint64_t state_false_mask,
    std::uint64_t required_true_mask,
    std::uint64_t required_false_mask
) noexcept {
    return (state_true_mask & required_true_mask) == required_true_mask &&
           (state_false_mask & required_false_mask) == required_false_mask;
}

[[nodiscard]] inline std::uint64_t bitmask_without_invalid(
    std::uint64_t mask,
    std::uint64_t invalid_mask
) noexcept {
    return mask & ~invalid_mask;
}

}  // namespace trading_engine::common::math
