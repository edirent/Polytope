#include "engine/reward/filter/RewardMarketFilter.h"

namespace trading_engine::reward {

RewardMarketFilter::RewardMarketFilter(RewardMarketFilterPolicy policy)
    : policy_(policy) {}

bool RewardMarketFilter::keep(
    const RewardMarketFeature& feature
) const noexcept {
    if (policy_.require_active && !feature.active) {
        return false;
    }
    if (policy_.require_valid_tokens && !feature.has_valid_tokens) {
        return false;
    }
    if (policy_.require_positive_rate && !feature.has_positive_rate) {
        return false;
    }
    if (policy_.require_reward_constraints &&
        !feature.has_reward_constraints) {
        return false;
    }
    if (feature.total_daily_rate < policy_.min_total_daily_rate) {
        return false;
    }
    return true;
}

}  // namespace trading_engine::reward
