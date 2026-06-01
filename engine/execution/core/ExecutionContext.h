#pragma once

#include "engine/execution/public/ExecutionConfig.h"
#include "engine/state/MarketStateSnapshot.h"
#include "state/view/MarketDepthView.h"

#include <cstdint>
#include <vector>

namespace trading_engine::execution {

struct ExecutionContext {
    std::uint64_t now_ns = 0;
    std::vector<state::MarketStateSnapshot> snapshots;
    const state::MarketDepthView* depth_views = nullptr;
    std::uint16_t depth_view_count = 0;
    ExecutionConfig config;
};

}  // namespace trading_engine::execution
