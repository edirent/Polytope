#include "engine/risk/guards/MaxLossGuard.h"

#include "oracle/public/CandidateBundle.h"

#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] MaxLossGuardResult reject(
    RiskDecisionType type,
    std::string reason,
    MaxLossGuardResult result
) {
    result.pass = false;
    result.rejection = type;
    result.reason = std::move(reason);
    return result;
}

}  // namespace

MaxLossGuardResult MaxLossGuard::check(
    const signal::OpportunityIntent& intent,
    const CostRevalidationResult& cost,
    const RiskPolicySnapshot& policy
) const {
    MaxLossGuardResult result;

    if (!cost.ok) {
        return reject(
            RiskDecisionType::RejectInternalError,
            "invalid cost revalidation result",
            result
        );
    }
    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        if (intent.legs[i].side != trading_engine::oracle::Side::Buy) {
            return reject(
                RiskDecisionType::RejectInternalError,
                "SELL leg max loss unsupported in risk v0",
                result
            );
        }
    }

    result.max_loss_tick = cost.risk_total_cost_tick;
    if (policy.max_total_cost_tick > 0 &&
        result.max_loss_tick > policy.max_total_cost_tick) {
        return reject(
            RiskDecisionType::RejectCostLimit,
            "risk total cost exceeds policy max",
            result
        );
    }

    result.pass = true;
    result.rejection = RiskDecisionType::Approve;
    return result;
}

}  // namespace trading_engine::risk
