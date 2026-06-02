#pragma once

#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/public/MarketMakingResult.h"

namespace trading_engine::strategy::market_making::tools {

state::MarketDepthView make_depth_view(
    std::int64_t bid_tick,
    std::int64_t ask_tick,
    double bid_size,
    double ask_size,
    std::uint64_t version,
    std::uint64_t now_ns
);

MarketMakingResult run_small_workflow(bool check_determinism);

}  // namespace trading_engine::strategy::market_making::tools
