#pragma once

#include "engine/reward/public/RewardTypes.h"
#include "engine/state/view/MarketDepthView.h"

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
    bool disable_bid_quotes = false;
    bool disable_ask_quotes = false;
    const reward::RewardConfigSnapshot* reward_config = nullptr;
    std::uint64_t now_ns = 0;
    std::uint64_t time_to_expiry_ns = 0;
};

}  // namespace trading_engine::strategy::market_making
