#pragma once

#include "engine/paper/pnl/RewardPnLTypes.h"
#include "engine/reward/public/RewardTypes.h"

#include <cstdint>
#include <span>

namespace trading_engine::paper {

class RewardPnLEngine {
public:
    [[nodiscard]] RewardPnLSnapshot compute(
        const reward::RewardConfigSnapshot& reward_config,
        std::span<const RewardQuoteObservation> observations,
        const RewardAccountSnapshot* account_snapshot = nullptr,
        std::uint64_t ts_ns = 0
    ) const;
};

}  // namespace trading_engine::paper
