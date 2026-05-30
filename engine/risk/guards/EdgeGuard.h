#pragma once

#include "engine/risk/guards/IRiskGuard.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/signal/public/OpportunityIntent.h"

namespace trading_engine::risk {

struct EdgeGuardResult {
    bool pass = false;
    RiskDecisionType rejection = RiskDecisionType::RejectInternalError;

    std::int64_t post_risk_edge_tick = 0;
    std::int64_t unit_edge_tick = 0;
    std::int64_t edge_bps = 0;

    std::string reason;
};

class EdgeGuard {
public:
    [[nodiscard]] EdgeGuardResult check(
        const signal::OpportunityIntent& intent,
        const CostRevalidationResult& cost,
        const RiskPolicySnapshot& policy
    ) const;
};

}  // namespace trading_engine::risk
