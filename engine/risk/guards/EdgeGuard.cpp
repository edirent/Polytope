#include "engine/risk/guards/EdgeGuard.h"

#include <limits>
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

[[nodiscard]] bool checked_mul_i64(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) * static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max() ||
        value < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] bool checked_sub_i64(
    std::int64_t rhs,
    std::int64_t* value
) noexcept {
    const auto next =
        static_cast<__int128>(*value) - static_cast<__int128>(rhs);
    if (next > std::numeric_limits<std::int64_t>::max() ||
        next < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *value = static_cast<std::int64_t>(next);
    return true;
}

[[nodiscard]] std::int64_t edge_bps(
    std::int64_t edge_tick,
    std::int64_t cost_tick
) noexcept {
    if (cost_tick == 0) {
        if (edge_tick > 0) {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (edge_tick < 0) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return 0;
    }

    const auto value =
        static_cast<__int128>(edge_tick) * 10'000 /
        static_cast<__int128>(cost_tick);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value < std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
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

    std::int64_t guaranteed_payout = 0;
    if (!checked_mul_i64(
            guaranteed_payout_per_bundle,
            cost.risk_bundle_qty,
            &guaranteed_payout
        )) {
        return reject(
            RiskDecisionType::RejectInternalError,
            "post-risk payout overflow",
            result
        );
    }

    result.post_risk_edge_tick = guaranteed_payout;
    if (!checked_sub_i64(cost.risk_total_cost_tick, &result.post_risk_edge_tick) ||
        !checked_sub_i64(cost.fee_tick, &result.post_risk_edge_tick) ||
        !checked_sub_i64(cost.slippage_buffer_tick, &result.post_risk_edge_tick) ||
        !checked_sub_i64(cost.latency_buffer_tick, &result.post_risk_edge_tick)) {
        return reject(
            RiskDecisionType::RejectInternalError,
            "post-risk edge overflow",
            result
        );
    }

    result.unit_edge_tick =
        result.post_risk_edge_tick / cost.risk_bundle_qty;
    result.edge_bps = edge_bps(
        result.post_risk_edge_tick,
        cost.risk_total_cost_tick
    );

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

    result.pass = true;
    result.rejection = RiskDecisionType::Approve;
    return result;
}

}  // namespace trading_engine::risk
