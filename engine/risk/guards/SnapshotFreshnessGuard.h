#pragma once

#include "engine/risk/guards/IRiskGuard.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/state/MarketStateSnapshot.h"
#include "engine/state/view/MarketDepthView.h"

namespace trading_engine::risk {

class SnapshotFreshnessGuard {
public:
    [[nodiscard]] GuardResult check(
        const state::MarketStateSnapshot& snapshot,
        const signal::OpportunityIntent& intent,
        const RiskPolicySnapshot& policy,
        std::uint64_t now_ns
    ) const;

    [[nodiscard]] GuardResult check(
        const state::MarketDepthView& depth_view,
        const signal::OpportunityIntent& intent,
        const RiskPolicySnapshot& policy,
        std::uint64_t now_ns
    ) const;
};

}  // namespace trading_engine::risk
