#pragma once

#include "engine/risk/guards/IRiskGuard.h"
#include "engine/state/MarketStateSnapshot.h"

namespace trading_engine::risk {

class MarketStateGuard {
public:
    [[nodiscard]] GuardResult check(
        const state::MarketStateSnapshot* snapshot
    ) const;
};

}  // namespace trading_engine::risk
