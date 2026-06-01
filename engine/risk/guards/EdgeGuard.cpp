#include "engine/risk/guards/EdgeGuard.h"

#include "engine/common/math/RiskMath.h"

#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] EdgeGuardResult reject(
    RiskDecisionType type,
    std::string reason,
    EdgeGuardResult result
) {
    result.pass = false;
    result.rejection = type;
    result.reason = std::move(reason);
    return result;
}

}  // namespace

EdgeGuardResult EdgeGuard::check(
    const signal::OpportunityIntent& intent,
    const CostRevalidationResult& cost,
    const RiskPolicySnapshot& policy
) const {
    EdgeGuardResult result;

    if (!cost.ok || cost.risk_bundle_qty <= 0) {
        return reject(
            RiskDecisionType::RejectInternalError,
            "invalid cost revalidation result",
            result
        );
    }

    const auto guaranteed_payout_per_bundle =
        intent.bundle_qty > 0
            ? intent.guaranteed_payout_tick / intent.bundle_qty
            : intent.guaranteed_payout_tick;

    const auto edge = trading_engine::common::math::compute_post_risk_edge({
        .guaranteed_payout_per_bundle_tick = guaranteed_payout_per_bundle,
        .risk_bundle_qty = cost.risk_bundle_qty,
        .risk_total_cost_tick = cost.risk_total_cost_tick,
        .fee_tick = cost.fee_tick,
        .slippage_buffer_tick = cost.slippage_buffer_tick,
        .latency_buffer_tick = cost.latency_buffer_tick
    });
    if (!edge.ok) {
        return reject(
            RiskDecisionType::RejectInternalError,
            "post-risk edge overflow",
            result
        );
    }

    result.post_risk_edge_tick = edge.post_risk_edge_tick;
    result.unit_edge_tick = edge.unit_edge_tick;
    result.edge_bps = edge.edge_bps;

    const auto thresholds_pass =
        trading_engine::common::math::passes_edge_thresholds(
            result.unit_edge_tick,
            result.post_risk_edge_tick,
            result.edge_bps,
            policy
        );
    if (!thresholds_pass) {
        if (result.post_risk_edge_tick <
            policy.min_post_risk_total_edge_tick) {
            return reject(
                RiskDecisionType::RejectLowTotalEdge,
                "post-risk total edge below threshold",
                result
            );
        }
        if (result.unit_edge_tick < policy.min_post_risk_unit_edge_tick) {
            return reject(
                RiskDecisionType::RejectLowUnitEdge,
                "post-risk unit edge below threshold",
                result
            );
        }
        if (result.edge_bps < policy.min_edge_bps) {
            return reject(
                RiskDecisionType::RejectLowEdgeBps,
                "post-risk edge bps below threshold",
                result
            );
        }
    }

    result.pass = true;
    result.rejection = RiskDecisionType::Approve;
    return result;
}

}  // namespace trading_engine::risk
