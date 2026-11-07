#pragma once

#include "engine/reward/public/RewardTypes.h"
#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

struct MarketMakingInput {
    std::string market_id;
    std::string asset_id;
    std::uint32_t market_index = 0;
    std::uint32_t asset_index = 0;
    const state::MarketDepthView* depth = nullptr;
    const state::MarketDepthView* complement_depth = nullptr;
    std::int64_t current_position_lots = 0;
    std::int64_t external_fair_value_tick = 0;
    std::int64_t dynamic_target_position_lots = 0;
    std::int64_t dynamic_min_inventory_lots = 0;
    std::int64_t dynamic_max_inventory_lots = 0;
    std::int64_t canonical_yes_fair_value_tick = 0;
    std::int64_t canonical_yes_position_lots = 0;
    std::int64_t target_canonical_yes_lots = 0;
    std::int64_t dynamic_half_spread_tick = 0;
    std::int64_t dynamic_min_half_spread_tick = 0;
    std::int64_t dynamic_max_inventory_skew_tick = 0;
    std::uint64_t dynamic_quote_ttl_ns = 0;
    std::int64_t dynamic_requote_threshold_tick = 0;
    std::int64_t dynamic_max_book_age_ns = 0;
    bool disable_bid_quotes = false;
    bool disable_ask_quotes = false;
    const reward::RewardConfigSnapshot* reward_config = nullptr;
    ProductKind product_kind = ProductKind::Generic;
    ProductDirection product_asset_direction = ProductDirection::Unknown;
    ProductDirection product_winning_direction = ProductDirection::Unknown;
    std::int64_t product_external_fair_tick = 0;
    std::int64_t product_z_score_bps = 0;
    std::uint64_t product_tau_ms = 0;
    std::int64_t product_dynamic_inventory_cap_lots = 0;
    bool product_opening_allowed = true;
    bool product_settlement_source_ok = true;
    bool product_reward_size_lift_allowed = true;
    bool product_reward_spread_compression_allowed = true;
    std::uint64_t now_ns = 0;
    std::uint64_t time_to_expiry_ns = 0;
};

}  // namespace trading_engine::strategy::market_making
