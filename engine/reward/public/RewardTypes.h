#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::reward {

enum class RewardSourceQuality : std::uint8_t {
    Unknown,
    Good,
    Stale,
    Unavailable,
    ParseError,
};

enum class RewardAsset : std::uint8_t {
    Unknown,
    Usdc,
    Other,
};

struct RewardTokenConfig {
    std::string token_id;
    std::string outcome;
    bool eligible = false;
};

struct RewardMarketConfig {
    std::string condition_id;
    std::string market_slug;

    RewardAsset reward_asset = RewardAsset::Unknown;
    std::string reward_asset_symbol;

    std::int64_t rewards_max_spread_tick = 0;
    std::int64_t rewards_min_size_lots = 0;

    double rate_per_day = 0.0;
    double native_daily_rate = 0.0;
    double sponsored_daily_rate = 0.0;
    double total_daily_rate = 0.0;

    std::uint64_t start_ts_ns = 0;
    std::uint64_t end_ts_ns = 0;

    bool active = false;
    bool sponsored = false;

    std::vector<RewardTokenConfig> tokens;
};

struct RewardConfigSnapshot {
    std::uint64_t snapshot_ts_ns = 0;
    std::uint64_t refresh_ts_ns = 0;
    std::uint64_t source_version_hash = 0;
    RewardSourceQuality source_quality = RewardSourceQuality::Unknown;
    std::string source;

    std::vector<RewardMarketConfig> markets;
};

struct RewardMarketFeature {
    std::string condition_id;
    std::uint32_t token_count = 0;
    std::int64_t rewards_max_spread_tick = 0;
    std::int64_t rewards_min_size_lots = 0;
    double total_daily_rate = 0.0;
    bool active = false;
    bool has_valid_tokens = false;
    bool has_positive_rate = false;
    bool has_reward_constraints = false;
};

[[nodiscard]] const char* reward_source_quality_name(
    RewardSourceQuality quality
) noexcept;

[[nodiscard]] const char* reward_asset_name(RewardAsset asset) noexcept;

[[nodiscard]] std::uint64_t compute_reward_config_hash(
    const RewardConfigSnapshot& snapshot
) noexcept;

}  // namespace trading_engine::reward
