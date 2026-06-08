#include "engine/reward/features/RewardMarketFeatureBuilder.h"

namespace trading_engine::reward {

RewardMarketFeature RewardMarketFeatureBuilder::build(
    const RewardMarketConfig& config
) const noexcept {
    RewardMarketFeature feature;
    feature.condition_id = config.condition_id;
    feature.token_count = static_cast<std::uint32_t>(config.tokens.size());
    feature.rewards_max_spread_tick = config.rewards_max_spread_tick;
    feature.rewards_min_size_lots = config.rewards_min_size_lots;
    feature.total_daily_rate = config.total_daily_rate;
    feature.active = config.active;
    feature.has_valid_tokens = false;
    for (const auto& token : config.tokens) {
        if (token.eligible && !token.token_id.empty()) {
            feature.has_valid_tokens = true;
            break;
        }
    }
    feature.has_positive_rate = config.total_daily_rate > 0.0 ||
        config.rate_per_day > 0.0;
    feature.has_reward_constraints =
        config.rewards_max_spread_tick > 0 &&
        config.rewards_min_size_lots > 0;
    return feature;
}

std::vector<RewardMarketFeature> RewardMarketFeatureBuilder::build_all(
    const RewardConfigSnapshot& snapshot
) const {
    std::vector<RewardMarketFeature> features;
    features.reserve(snapshot.markets.size());
    for (const auto& config : snapshot.markets) {
        features.push_back(build(config));
    }
    return features;
}

}  // namespace trading_engine::reward
