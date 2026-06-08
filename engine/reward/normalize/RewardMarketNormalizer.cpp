#include "engine/reward/normalize/RewardMarketNormalizer.h"

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace trading_engine::reward {
namespace {

[[nodiscard]] std::string lowercase(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))
        );
    }
    return value;
}

}  // namespace

RewardAsset normalize_reward_asset(const std::string& symbol) noexcept {
    const auto lowered = lowercase(symbol);
    if (lowered == "usdc" || lowered == "usd coin") {
        return RewardAsset::Usdc;
    }
    if (!lowered.empty()) {
        return RewardAsset::Other;
    }
    return RewardAsset::Unknown;
}

std::int64_t reward_spread_to_tick(double rewards_max_spread) noexcept {
    if (!std::isfinite(rewards_max_spread) || rewards_max_spread <= 0.0) {
        return 0;
    }
    // Polymarket payloads have historically exposed spread caps as decimal
    // probabilities (0.035) or cents/percent-like values (3.5). Accept both.
    const auto decimal = rewards_max_spread > 1.0
        ? rewards_max_spread / 100.0
        : rewards_max_spread;
    const auto tick = decimal *
        static_cast<double>(strategy::market_making::kPriceOneTick);
    return std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(std::llround(tick))
    );
}

std::int64_t reward_min_size_to_lots(double rewards_min_size) noexcept {
    if (!std::isfinite(rewards_min_size) || rewards_min_size <= 0.0) {
        return 0;
    }
    return static_cast<std::int64_t>(std::ceil(rewards_min_size));
}

RewardMarketConfig RewardMarketNormalizer::normalize(
    const RawRewardMarketConfig& raw
) const {
    RewardMarketConfig config;
    config.condition_id = raw.condition_id;
    config.market_slug = raw.market_slug;
    config.reward_asset_symbol = raw.reward_asset_symbol;
    config.reward_asset = normalize_reward_asset(raw.reward_asset_symbol);
    config.rewards_max_spread_tick =
        reward_spread_to_tick(raw.rewards_max_spread);
    config.rewards_min_size_lots =
        reward_min_size_to_lots(raw.rewards_min_size);
    config.rate_per_day = raw.rate_per_day;
    config.native_daily_rate = raw.native_daily_rate;
    config.sponsored_daily_rate = raw.sponsored_daily_rate;
    config.total_daily_rate = raw.total_daily_rate > 0.0
        ? raw.total_daily_rate
        : raw.native_daily_rate + raw.sponsored_daily_rate;
    config.start_ts_ns = raw.start_ts_ns;
    config.end_ts_ns = raw.end_ts_ns;
    config.active = raw.active;
    config.sponsored = raw.sponsored;
    config.tokens.reserve(raw.tokens.size());
    for (const auto& token : raw.tokens) {
        config.tokens.push_back(
            RewardTokenConfig{
                .token_id = token.token_id,
                .outcome = token.outcome,
                .eligible = !token.token_id.empty(),
            }
        );
    }
    return config;
}

RewardConfigSnapshot RewardMarketNormalizer::normalize_snapshot(
    std::vector<RawRewardMarketConfig> raw_markets,
    std::uint64_t refresh_ts_ns,
    std::string source
) const {
    RewardConfigSnapshot snapshot;
    snapshot.snapshot_ts_ns = refresh_ts_ns;
    snapshot.refresh_ts_ns = refresh_ts_ns;
    snapshot.source_quality = RewardSourceQuality::Good;
    snapshot.source = std::move(source);
    snapshot.markets.reserve(raw_markets.size());
    for (const auto& raw : raw_markets) {
        snapshot.markets.push_back(normalize(raw));
    }
    snapshot.source_version_hash = compute_reward_config_hash(snapshot);
    return snapshot;
}

}  // namespace trading_engine::reward
