#include "engine/risk/reprice/CostRevalidator.h"

namespace trading_engine::risk {

CostRevalidationResult CostRevalidator::revalidate(
    const signal::OpportunityIntent& intent,
    const std::vector<state::MarketStateSnapshot>& snapshots,
    const RiskPolicySnapshot& policy
) const {
    auto result = vwap_.reprice(intent, snapshots);
    if (!result.ok) {
        return result;
    }

    result.fee_tick = fee_.estimate_fee_tick(intent);
    result.slippage_buffer_tick =
        slippage_.estimate_slippage_buffer_tick(intent);
    result.latency_buffer_tick = latency_.estimate_latency_buffer_tick(intent);
    result.cost_drift_tick =
        result.risk_total_cost_tick - intent.estimated_cost_tick;

    if (result.cost_drift_tick > policy.max_allowed_cost_drift_tick) {
        result.ok = false;
        result.rejection = RiskDecisionType::RejectCostDrift;
        result.reason = "cost drift exceeds policy";
        return result;
    }

    result.ok = true;
    result.rejection = RiskDecisionType::Approve;
    return result;
}

}  // namespace trading_engine::risk
