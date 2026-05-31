#pragma once

#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/MarketStateSnapshot.h"

#include <cstddef>
#include <vector>

namespace trading_engine::risk {

class VWAPRevalidator {
public:
    [[nodiscard]] CostRevalidationResult reprice(
        const signal::OpportunityIntent& intent,
        const std::vector<state::MarketStateSnapshot>& snapshots
    ) const;

    [[nodiscard]] CostRevalidationResult reprice(
        const signal::OpportunityIntent& intent,
        const state::MarketStateSnapshot* snapshots,
        std::size_t snapshot_count
    ) const;
};

}  // namespace trading_engine::risk
