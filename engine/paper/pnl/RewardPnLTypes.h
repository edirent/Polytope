#pragma once

#include "engine/reward/public/RewardTypes.h"
#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::paper {

struct RewardQuoteObservation {
    std::string condition_id;
    std::string asset_id;
    std::uint32_t asset_index = 0;

    bool has_bid = false;
    bool has_ask = false;
    std::int64_t bid_price_tick = 0;
    std::int64_t ask_price_tick = 0;
    std::int64_t bid_size_lots = 0;
    std::int64_t ask_size_lots = 0;

    std::uint64_t start_ts_ns = 0;
    std::uint64_t end_ts_ns = 0;
};

struct RewardAccountSnapshot {
    bool available = false;
    std::int64_t reconciled_reward_tick = 0;
    std::uint64_t ts_ns = 0;
};

struct RewardPnLSnapshot {
    std::uint64_t ts_ns = 0;
    std::int64_t reward_daily_rate_tick = 0;
    std::uint64_t eligible_quote_seconds = 0;
    std::int64_t eligible_notional_tick_seconds = 0;
    std::int64_t reward_accrued_tick_estimate = 0;
    std::int64_t reward_reconciled_tick = 0;
    std::uint64_t reward_config_age_ms = 0;
    reward::RewardSourceQuality reward_source_quality =
        reward::RewardSourceQuality::Unknown;
    std::uint32_t reward_eligible_market_count = 0;
    std::uint32_t observations_seen = 0;
    std::uint32_t eligible_observations = 0;
};

}  // namespace trading_engine::paper
