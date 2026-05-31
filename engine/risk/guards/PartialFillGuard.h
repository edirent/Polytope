#pragma once

#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>
#include <string>

namespace trading_engine::risk {

struct PartialFillGuardResult {
    bool pass = false;
    RiskDecisionType rejection = RiskDecisionType::RejectInternalError;

    bool depth_margin_checked = false;
    bool unhedged_loss_evaluated = false;
    bool unhedged_loss_placeholder_available = true;

    std::string rejected_asset_id;
    std::int64_t requested_qty_lots = 0;
    std::int64_t available_depth_lots = 0;
    std::int64_t required_depth_with_margin_lots = 0;
    std::int64_t max_unhedged_loss_tick = 0;

    std::string reason;
};

class PartialFillGuard {
public:
    [[nodiscard]] PartialFillGuardResult check(
        const signal::OpportunityIntent& intent,
        const CostRevalidationResult& cost,
        const RiskPolicySnapshot& policy
    ) const;
};

}  // namespace trading_engine::risk
