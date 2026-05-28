#pragma once

#include <cstdint>
#include <vector>

namespace trading_engine::oracle {

struct FeasibleState {
    std::uint64_t state_id = 0;
    std::vector<std::uint64_t> bitset_words;
};

}  // namespace trading_engine::oracle
