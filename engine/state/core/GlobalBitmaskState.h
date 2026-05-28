#pragma once

#include <cstdint>

namespace trading_engine::state {

struct GlobalBitmaskState {
    std::uint64_t resolved_true_mask{0};
    std::uint64_t resolved_false_mask{0};
    std::uint64_t invalid_mask{0};
    std::uint64_t version{0};
};

}  // namespace trading_engine::state
