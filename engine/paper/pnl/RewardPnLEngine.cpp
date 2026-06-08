#include "engine/paper/pnl/RewardPnLEngine.h"

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <algorithm>
#include <cmath>

namespace trading_engine::paper {
namespace {

inline constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;
inline constexpr std::uint64_t kSecondsPerDay = 86'400ULL;

[[nodiscard]] const reward::RewardMarketConfig* find_market(
    const reward::RewardConfigSnapshot& snapshot,
    const RewardQuoteObservation& observation
) noexcept {
    for (const auto& market : snapshot.markets) {
        if (!observation.condition_id.empty() &&
            market.condition_id == observation.condition_id) {
            return &market;
        }
        for (const auto& token : market.tokens) {
            if (token.eligible && token.token_id == observation.asset_id) {
                return &market;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] bool has_token(
    const reward::RewardMarketConfig& market,
    const std::string& asset_id
) noexcept {
    for (const auto& token : market.tokens) {
        if (token.eligible && token.token_id == asset_id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::int64_t daily_rate_tick(
    const reward::RewardMarketConfig& market
) noexcept {
    const auto rate = market.total_daily_rate > 0.0
        ? market.total_daily_rate
        : market.rate_per_day;
    if (!std::isfinite(rate) || rate <= 0.0) {
        return 0;
    }
    return static_cast<std::int64_t>(
        std::llround(
            rate * strategy::market_making::kPriceOneTick
        )
    );
}

[[nodiscard]] bool observation_eligible(
    const reward::RewardMarketConfig& market,
    const RewardQuoteObservation& observation
) noexcept {
    if (!market.active || !has_token(market, observation.asset_id)) {
        return false;
    }
    if (!observation.has_bid || !observation.has_ask ||
        observation.bid_price_tick <= 0 || observation.ask_price_tick <= 0 ||
        observation.bid_price_tick >= observation.ask_price_tick) {
        return false;
    }
    if (market.rewards_max_spread_tick > 0 &&
        observation.ask_price_tick - observation.bid_price_tick >
            market.rewards_max_spread_tick) {
        return false;
    }
    if (market.rewards_min_size_lots > 0 &&
        (observation.bid_size_lots < market.rewards_min_size_lots ||
         observation.ask_size_lots < market.rewards_min_size_lots)) {
        return false;
    }
    return observation.end_ts_ns > observation.start_ts_ns;
}

[[nodiscard]] std::int64_t notional_tick(
    const RewardQuoteObservation& observation
) noexcept {
    const auto bid_notional = static_cast<__int128>(
        observation.bid_price_tick
    ) * observation.bid_size_lots;
    const auto ask_notional = static_cast<__int128>(
        observation.ask_price_tick
    ) * observation.ask_size_lots;
    const auto total = bid_notional + ask_notional;
    return total > static_cast<__int128>(INT64_MAX)
        ? INT64_MAX
        : static_cast<std::int64_t>(total);
}

}  // namespace

RewardPnLSnapshot RewardPnLEngine::compute(
    const reward::RewardConfigSnapshot& reward_config,
    std::span<const RewardQuoteObservation> observations,
    const RewardAccountSnapshot* account_snapshot,
    std::uint64_t ts_ns
) const {
    RewardPnLSnapshot snapshot;
    snapshot.ts_ns = ts_ns;
    snapshot.reward_source_quality = reward_config.source_quality;
    if (ts_ns > reward_config.refresh_ts_ns && reward_config.refresh_ts_ns > 0) {
        snapshot.reward_config_age_ms =
            (ts_ns - reward_config.refresh_ts_ns) / 1'000'000ULL;
    }

    for (const auto& market : reward_config.markets) {
        if (market.active && daily_rate_tick(market) > 0 &&
            !market.tokens.empty()) {
            ++snapshot.reward_eligible_market_count;
            snapshot.reward_daily_rate_tick += daily_rate_tick(market);
        }
    }

    for (const auto& observation : observations) {
        ++snapshot.observations_seen;
        const auto* market = find_market(reward_config, observation);
        if (!market || !observation_eligible(*market, observation)) {
            continue;
        }
        ++snapshot.eligible_observations;
        const auto duration_sec =
            (observation.end_ts_ns - observation.start_ts_ns) / kNsPerSecond;
        snapshot.eligible_quote_seconds += duration_sec;
        snapshot.eligible_notional_tick_seconds +=
            notional_tick(observation) *
            static_cast<std::int64_t>(duration_sec);
        snapshot.reward_accrued_tick_estimate +=
            daily_rate_tick(*market) *
            static_cast<std::int64_t>(duration_sec) /
            static_cast<std::int64_t>(kSecondsPerDay);
    }

    if (account_snapshot && account_snapshot->available) {
        snapshot.reward_reconciled_tick =
            account_snapshot->reconciled_reward_tick;
    }
    return snapshot;
}

}  // namespace trading_engine::paper
