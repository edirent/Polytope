#pragma once

#include "engine/reward/public/RewardTypes.h"

#include <vector>

namespace trading_engine::reward {

class RewardMarketFeatureBuilder {
public:
    [[nodiscard]] RewardMarketFeature build(
        const RewardMarketConfig& config
    ) const noexcept;

    [[nodiscard]] std::vector<RewardMarketFeature> build_all(
        const RewardConfigSnapshot& snapshot
    ) const;
};

}  // namespace trading_engine::reward
