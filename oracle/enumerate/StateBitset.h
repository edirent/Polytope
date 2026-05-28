#pragma once

#include <cstdint>
#include <vector>

namespace trading_engine::oracle {

struct StateBitset {
    std::vector<std::uint64_t> words;

    [[nodiscard]] bool test(std::uint32_t var_id) const noexcept {
        const std::uint32_t word_index = var_id / 64U;
        const std::uint32_t bit_index = var_id % 64U;
        if (word_index >= words.size()) {
            return false;
        }
        return ((words[word_index] >> bit_index) & 1ULL) != 0;
    }
};

[[nodiscard]] inline StateBitset state_bitset_from_mask(
    std::uint64_t mask
) {
    return StateBitset{{mask}};
}

}  // namespace trading_engine::oracle
