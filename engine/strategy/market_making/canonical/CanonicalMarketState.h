#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

struct CanonicalMarketState {
    std::string market_id;
    std::string token_id;
    std::string complement_token_id;

    ExternalFairEventType event_type = ExternalFairEventType::Unknown;
    OutcomeSide asset_side = OutcomeSide::Yes;

    std::int64_t book_bid_tick = 0;
    std::int64_t book_ask_tick = 0;
    std::int64_t book_mid_tick = 0;
    std::int64_t spread_tick = 0;

    std::int64_t canonical_yes_bid_tick = 0;
    std::int64_t canonical_yes_ask_tick = 0;
    std::int64_t canonical_yes_mid_tick = 0;

    std::int64_t asset_mid_tick = 0;
    std::int64_t asset_external_fair_tick = 0;
    std::int64_t asset_tradable_fair_tick = 0;
    std::int64_t canonical_yes_external_fair_tick = 0;
    std::int64_t canonical_yes_tradable_fair_tick = 0;

    double spot = 0.0;
    double annualized_vol = 0.0;
    std::int64_t tte_ns = 0;

    std::int64_t book_age_ms = 0;
    std::int64_t spot_age_ms = 0;
    std::int64_t vol_age_ms = 0;
    std::int64_t current_asset_position_lots = 0;
    std::int64_t current_canonical_yes_position_lots = 0;
};

}  // namespace trading_engine::strategy::market_making
