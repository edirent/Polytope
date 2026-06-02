#pragma once

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct FairValueBlendInput {
    std::int64_t primary_tick = 0;
    std::int64_t secondary_tick = 0;
    std::int64_t secondary_weight_bps = 0;
};

[[nodiscard]] inline std::int64_t blend_fair_value(
    const FairValueBlendInput& input
) noexcept {
    if (input.secondary_weight_bps <= 0) {
        return input.primary_tick;
    }
    if (input.secondary_weight_bps >= 10'000) {
        return input.secondary_tick;
    }
    return (input.primary_tick * (10'000 - input.secondary_weight_bps) +
            input.secondary_tick * input.secondary_weight_bps) /
           10'000;
}

}  // namespace trading_engine::strategy::market_making
