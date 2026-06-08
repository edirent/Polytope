#pragma once

#include "engine/reward/public/RewardTypes.h"

#include <cstdint>

namespace trading_engine::reward {

struct RewardMarketFilterPolicy {
    bool require_active = true;
    bool require_valid_tokens = true;
    bool require_positive_rate = true;
    bool require_reward_constraints = true;
    double min_total_daily_rate = 0.0;
};

class RewardMarketFilter {
public:
    explicit RewardMarketFilter(RewardMarketFilterPolicy policy = {});

    [[nodiscard]] bool keep(const RewardMarketFeature& feature) const noexcept;

private:
    RewardMarketFilterPolicy policy_;
};

}  // namespace trading_engine::reward
