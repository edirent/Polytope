#pragma once

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
    std::int64_t current_position_lots = 0;
    std::uint64_t now_ns = 0;
};

}  // namespace trading_engine::strategy::market_making
