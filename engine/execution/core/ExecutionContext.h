#pragma once

#include "engine/execution/public/ExecutionConfig.h"
#include "engine/state/MarketStateSnapshot.h"

#include <cstdint>
#include <vector>

namespace trading_engine::execution {

struct ExecutionContext {
    std::uint64_t now_ns = 0;
    std::vector<state::MarketStateSnapshot> snapshots;
    ExecutionConfig config;
};

}  // namespace trading_engine::execution
