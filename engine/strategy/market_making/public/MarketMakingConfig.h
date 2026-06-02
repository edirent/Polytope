#pragma once

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct MarketMakingConfig {
    std::uint64_t strategy_id = 1;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t policy_hash = 0;

    std::int64_t min_price_tick = 1;
    std::int64_t max_price_tick = kPriceOneTick - 1;

    std::int64_t min_half_spread_tick = 1'000;
    std::int64_t fee_buffer_tick = 0;
    std::int64_t latency_buffer_tick = 0;
    std::int64_t volatility_buffer_tick = 0;
    std::int64_t liquidity_buffer_tick = 0;
    std::int64_t uncertainty_buffer_tick = 0;

    std::int64_t max_inventory_skew_tick = 0;
    std::int64_t target_position_lots = 0;
    std::int64_t min_inventory_lots = 0;
    std::int64_t max_inventory_lots = 100;
    std::int64_t base_quote_size_lots = 1;

    std::uint64_t quote_ttl_ns = 5'000'000'000ULL;
    std::int64_t requote_threshold_tick = 1'000;
    std::int64_t inventory_requote_threshold_lots = 1;
    std::int64_t max_book_age_ns = 1'000'000'000LL;

    bool enable_bid_quotes = true;
    bool enable_ask_quotes = true;
};

}  // namespace trading_engine::strategy::market_making
