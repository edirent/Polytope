#pragma once

#include "engine/common/math/FixedPointMath.h"

#include <cstdint>

namespace trading_engine::common::math {

struct EdgeMathInput {
    std::int64_t guaranteed_payout_tick = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t fee_tick = 0;
    std::int64_t latency_buffer_tick = 0;
    std::int64_t slippage_buffer_tick = 0;
    std::int64_t bundle_qty = 0;
};

struct EdgeMathResult {
    bool ok = true;
    std::int64_t unit_edge_tick = 0;
    std::int64_t total_edge_tick = 0;
    std::int64_t edge_bps = 0;
};

[[nodiscard]] inline EdgeMathResult compute_edge(
    const EdgeMathInput& input
) noexcept {
    EdgeMathResult result;
    if (!checked_sub_i64(
            input.guaranteed_payout_tick,
            input.total_cost_tick,
            &result.unit_edge_tick
        ) ||
        !checked_sub_i64(
            result.unit_edge_tick,
            input.fee_tick,
            &result.unit_edge_tick
        ) ||
        !checked_sub_i64(
            result.unit_edge_tick,
            input.latency_buffer_tick,
            &result.unit_edge_tick
        ) ||
        !checked_sub_i64(
            result.unit_edge_tick,
            input.slippage_buffer_tick,
            &result.unit_edge_tick
        )) {
        result.ok = false;
        return result;
    }

    if (!checked_mul_i64(
            result.unit_edge_tick,
            input.bundle_qty,
            &result.total_edge_tick
        )) {
        result.ok = false;
        return result;
    }

    result.edge_bps = ratio_bps(result.unit_edge_tick, input.total_cost_tick);
    return result;
}

}  // namespace trading_engine::common::math
