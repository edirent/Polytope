#pragma once

#include "engine/risk/public/RiskAuditTrace.h"
#include "engine/risk/reprice/CostRevalidationResult.h"

#include <array>
#include <cstdint>

namespace trading_engine::risk {

struct RiskScratch {
    std::array<RevalidatedLegCost, kMaxRevalidatedLegCosts>
        revalidated_legs{};
    std::uint16_t leg_count = 0;

    RiskAuditTraceLite audit_trace;

    void reset() noexcept {
        leg_count = 0;
        audit_trace = RiskAuditTraceLite{};
    }

    [[nodiscard]] bool push_revalidated_leg(const RevalidatedLegCost& leg) {
        if (leg_count >= revalidated_legs.size()) {
            return false;
        }
        revalidated_legs[leg_count++] = leg;
        return true;
    }
};

}  // namespace trading_engine::risk
