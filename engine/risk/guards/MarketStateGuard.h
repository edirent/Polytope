#pragma once

#include "engine/risk/guards/IRiskGuard.h"
#include "engine/state/MarketStateSnapshot.h"
#include "engine/state/view/MarketDepthView.h"

#include <cstddef>

namespace trading_engine::risk {

class MarketStateGuard {
public:
    [[nodiscard]] GuardResult check(
        const state::MarketStateSnapshot* snapshot
    ) const;

    [[nodiscard]] GuardResult check(
        const state::MarketDepthView* depth_view
    ) const;

    [[nodiscard]] GuardResult check(std::nullptr_t) const;
};

}  // namespace trading_engine::risk
