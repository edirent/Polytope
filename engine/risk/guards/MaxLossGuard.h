#pragma once

#include "engine/risk/guards/IRiskGuard.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/signal/public/OpportunityIntent.h"

namespace trading_engine::risk {

struct MaxLossGuardResult {
    bool pass = false;
    RiskDecisionType rejection = RiskDecisionType::RejectInternalError;
    std::int64_t max_loss_tick = 0;
    std::string reason;
};

class MaxLossGuard {
public:
    [[nodiscard]] MaxLossGuardResult check(
        const signal::OpportunityIntent& intent,
        const CostRevalidationResult& cost,
        const RiskPolicySnapshot& policy
    ) const;
};

}  // namespace trading_engine::risk
