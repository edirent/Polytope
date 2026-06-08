#pragma once

#include "engine/reward/public/RewardTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::reward {

struct RawRewardToken {
    std::string token_id;
    std::string outcome;
};

struct RawRewardMarketConfig {
    std::string condition_id;
    std::string market_slug;
    std::string reward_asset_symbol;

    double rewards_max_spread = 0.0;
    double rewards_min_size = 0.0;
    double rate_per_day = 0.0;
    double native_daily_rate = 0.0;
    double sponsored_daily_rate = 0.0;
    double total_daily_rate = 0.0;

    std::uint64_t start_ts_ns = 0;
    std::uint64_t end_ts_ns = 0;
    bool active = false;
    bool sponsored = false;

    std::vector<RawRewardToken> tokens;
};

class RewardMarketNormalizer {
public:
    [[nodiscard]] RewardMarketConfig normalize(
        const RawRewardMarketConfig& raw
    ) const;

    [[nodiscard]] RewardConfigSnapshot normalize_snapshot(
        std::vector<RawRewardMarketConfig> raw_markets,
        std::uint64_t refresh_ts_ns,
        std::string source
    ) const;
};

[[nodiscard]] RewardAsset normalize_reward_asset(
    const std::string& symbol
) noexcept;

[[nodiscard]] std::int64_t reward_spread_to_tick(
    double rewards_max_spread
) noexcept;

[[nodiscard]] std::int64_t reward_min_size_to_lots(
    double rewards_min_size
) noexcept;

}  // namespace trading_engine::reward
