#pragma once

#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/risk/reprice/FeeRevalidator.h"
#include "engine/risk/reprice/LatencyRevalidator.h"
#include "engine/risk/reprice/SlippageRevalidator.h"
#include "engine/risk/reprice/VWAPRevalidator.h"

namespace trading_engine::risk {

class CostRevalidator {
public:
    [[nodiscard]] CostRevalidationResult revalidate(
        const signal::OpportunityIntent& intent,
        const std::vector<state::MarketStateSnapshot>& snapshots,
        const RiskPolicySnapshot& policy
    ) const;

private:
    VWAPRevalidator vwap_;
    FeeRevalidator fee_;
    SlippageRevalidator slippage_;
    LatencyRevalidator latency_;
};

}  // namespace trading_engine::risk
