#pragma once

#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

inline constexpr std::int64_t kDefaultDefensiveHalfSpreadTick = 25'000;
inline constexpr std::int64_t kDefaultDefensiveQuoteSizeLots = 9;
inline constexpr std::int64_t kDefaultDefensiveInventorySkewTick = 75'000;

struct MarketMakingConfig {
    std::uint64_t strategy_id = 1;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t policy_hash = 0;

    std::int64_t min_price_tick = 1;
    std::int64_t max_price_tick = kPriceOneTick - 1;

    std::int64_t min_half_spread_tick = kDefaultDefensiveHalfSpreadTick;
    std::int64_t fee_buffer_tick = 0;
    std::int64_t latency_buffer_tick = 0;
    std::int64_t volatility_buffer_tick = 0;
    std::int64_t liquidity_buffer_tick = 0;
    std::int64_t uncertainty_buffer_tick = 0;

    std::int64_t max_inventory_skew_tick =
        kDefaultDefensiveInventorySkewTick;
    std::int64_t inventory_skew_nonlinear_start_bps = 10'000;
    double inventory_skew_exponent = 1.0;
    std::uint64_t tte_skew_start_ns = 0;
    std::uint64_t tte_puke_start_ns = 0;
    double tte_max_skew_multiplier = 1.0;
    std::int64_t target_position_lots = 0;
    std::int64_t min_inventory_lots = 0;
    std::int64_t max_inventory_lots = 100;
    std::int64_t base_quote_size_lots = kDefaultDefensiveQuoteSizeLots;
    std::int64_t min_quote_edge_tick = 0;
    std::int64_t adverse_selection_buffer_tick = 0;

    std::int64_t min_top_depth_lots = 1;
    std::int64_t min_total_depth_lots = 1;
    std::int64_t max_book_spread_tick = 0;
    std::int64_t max_book_spread_bps = 0;
    std::int64_t min_fair_confidence_bps = 6'000;
    std::int64_t fair_mid_weight_bps = 4'000;
    std::int64_t fair_microprice_weight_bps = 4'000;
    std::int64_t fair_vwap_weight_bps = 2'000;
    std::int64_t fair_vwap_lots = 10;
    std::int64_t complement_fair_weight_bps = 0;
    std::int64_t external_fair_value_tick = 0;
    std::int64_t external_fair_weight_bps = 0;
    bool require_external_fair_for_opening_quotes = false;

    std::int64_t max_quote_size_multiplier_bps = 20'000;
    std::int64_t passive_unwind_position_bps = 7'000;
    std::int64_t forced_unwind_position_bps = 9'000;
    std::int64_t passive_unwind_aggression_tick = 0;
    std::int64_t passive_reduce_excess_lots = 20;
    std::int64_t urgent_reduce_excess_lots = 50;
    std::int64_t passive_reduce_join_tick = 1;
    std::int64_t urgent_unwind_aggression_tick = 0;
    bool reduce_only_quote_to_target = true;

    std::uint64_t quote_ttl_ns = 5'000'000'000ULL;
    std::int64_t requote_threshold_tick = 1'000;
    std::int64_t min_quote_price_change_tick = 0;
    std::uint64_t min_requote_interval_ns = 0;
    std::int64_t inventory_requote_threshold_lots = 1;
    std::int64_t max_book_age_ns = 1'000'000'000LL;

    bool enable_bid_quotes = true;
    bool enable_ask_quotes = true;
};

}  // namespace trading_engine::strategy::market_making
