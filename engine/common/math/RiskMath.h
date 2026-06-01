#pragma once

#include "engine/common/math/FixedPointMath.h"
#include "engine/risk/public/RiskPolicySnapshot.h"

#include <cstdint>

namespace trading_engine::common::math {

struct RiskEdgeMathInput {
    std::int64_t guaranteed_payout_per_bundle_tick = 0;
    std::int64_t risk_bundle_qty = 0;
    std::int64_t risk_total_cost_tick = 0;
    std::int64_t fee_tick = 0;
    std::int64_t slippage_buffer_tick = 0;
    std::int64_t latency_buffer_tick = 0;
};

struct RiskEdgeMathResult {
    bool ok = false;
    std::int64_t post_risk_edge_tick = 0;
    std::int64_t unit_edge_tick = 0;
    std::int64_t edge_bps = 0;
};

[[nodiscard]] inline RiskEdgeMathResult compute_post_risk_edge(
    const RiskEdgeMathInput& input
) noexcept {
    RiskEdgeMathResult result;
    if (input.risk_bundle_qty <= 0) {
        return result;
    }

    if (!checked_mul_i64(
            input.guaranteed_payout_per_bundle_tick,
            input.risk_bundle_qty,
            &result.post_risk_edge_tick
        ) ||
        !checked_sub_i64(
            result.post_risk_edge_tick,
            input.risk_total_cost_tick,
            &result.post_risk_edge_tick
        ) ||
        !checked_sub_i64(
            result.post_risk_edge_tick,
            input.fee_tick,
            &result.post_risk_edge_tick
        ) ||
        !checked_sub_i64(
            result.post_risk_edge_tick,
            input.slippage_buffer_tick,
            &result.post_risk_edge_tick
        ) ||
        !checked_sub_i64(
            result.post_risk_edge_tick,
            input.latency_buffer_tick,
            &result.post_risk_edge_tick
        )) {
        return result;
    }

    result.unit_edge_tick = result.post_risk_edge_tick / input.risk_bundle_qty;
    result.edge_bps =
        ratio_bps(result.post_risk_edge_tick, input.risk_total_cost_tick);
    result.ok = true;
    return result;
}

[[nodiscard]] inline bool passes_edge_thresholds(
    std::int64_t unit_edge_tick,
    std::int64_t total_edge_tick,
    std::int64_t edge_bps,
    const trading_engine::risk::RiskPolicySnapshot& policy
) noexcept {
    return total_edge_tick >= policy.min_post_risk_total_edge_tick &&
           unit_edge_tick >= policy.min_post_risk_unit_edge_tick &&
           edge_bps >= policy.min_edge_bps;
}

}  // namespace trading_engine::common::math
