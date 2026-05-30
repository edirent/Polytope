#pragma once

#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/MarketStateSnapshot.h"

#include <vector>

namespace trading_engine::risk {

class VWAPRevalidator {
public:
    [[nodiscard]] CostRevalidationResult reprice(
        const signal::OpportunityIntent& intent,
        const std::vector<state::MarketStateSnapshot>& snapshots
    ) const;
};

}  // namespace trading_engine::risk
