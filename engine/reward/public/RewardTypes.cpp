#include "engine/reward/public/RewardTypes.h"

#include <bit>

namespace trading_engine::reward {
namespace {

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & 0xffU;
        *hash *= kFnvPrime;
    }
}

void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

void mix_bool(std::uint64_t* hash, bool value) noexcept {
    *hash ^= value ? 1U : 0U;
    *hash *= kFnvPrime;
}

void mix_double(std::uint64_t* hash, double value) noexcept {
    mix_u64(hash, std::bit_cast<std::uint64_t>(value));
}

void mix_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (const auto ch : value) {
        *hash ^= static_cast<unsigned char>(ch);
        *hash *= kFnvPrime;
    }
}

}  // namespace

const char* reward_source_quality_name(RewardSourceQuality quality) noexcept {
    switch (quality) {
        case RewardSourceQuality::Unknown:
            return "unknown";
        case RewardSourceQuality::Good:
            return "good";
        case RewardSourceQuality::Stale:
            return "stale";
        case RewardSourceQuality::Unavailable:
            return "unavailable";
        case RewardSourceQuality::ParseError:
            return "parse_error";
    }
    return "unknown";
}

const char* reward_asset_name(RewardAsset asset) noexcept {
    switch (asset) {
        case RewardAsset::Unknown:
            return "unknown";
        case RewardAsset::Usdc:
            return "usdc";
        case RewardAsset::Other:
            return "other";
    }
    return "unknown";
}

std::uint64_t compute_reward_config_hash(
    const RewardConfigSnapshot& snapshot
) noexcept {
    auto hash = kFnvOffset;
    mix_u64(&hash, snapshot.snapshot_ts_ns);
    mix_u64(&hash, snapshot.refresh_ts_ns);
    mix_u64(&hash, static_cast<std::uint8_t>(snapshot.source_quality));
    mix_string(&hash, snapshot.source);
    for (const auto& market : snapshot.markets) {
        mix_string(&hash, market.condition_id);
        mix_string(&hash, market.market_slug);
        mix_u64(&hash, static_cast<std::uint8_t>(market.reward_asset));
        mix_string(&hash, market.reward_asset_symbol);
        mix_i64(&hash, market.rewards_max_spread_tick);
        mix_i64(&hash, market.rewards_min_size_lots);
        mix_double(&hash, market.rate_per_day);
        mix_double(&hash, market.native_daily_rate);
        mix_double(&hash, market.sponsored_daily_rate);
        mix_double(&hash, market.total_daily_rate);
        mix_u64(&hash, market.start_ts_ns);
        mix_u64(&hash, market.end_ts_ns);
        mix_bool(&hash, market.active);
        mix_bool(&hash, market.sponsored);
        for (const auto& token : market.tokens) {
            mix_string(&hash, token.token_id);
            mix_string(&hash, token.outcome);
            mix_bool(&hash, token.eligible);
        }
    }
    return hash == 0 ? 1 : hash;
}

}  // namespace trading_engine::reward
