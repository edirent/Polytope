#pragma once

#include "engine/order_decision/math/EdgeCurve.h"

namespace trading_engine::order_decision {

[[nodiscard]] inline bool better_edge_candidate(
    const EdgeCurvePoint& candidate,
    const EdgeCurvePoint& incumbent
) noexcept {
    if (candidate.total_edge_tick != incumbent.total_edge_tick) {
        return candidate.total_edge_tick > incumbent.total_edge_tick;
    }
    if (candidate.edge_bps != incumbent.edge_bps) {
        return candidate.edge_bps > incumbent.edge_bps;
    }
    if (candidate.total_cost_tick != incumbent.total_cost_tick) {
        return candidate.total_cost_tick < incumbent.total_cost_tick;
    }
    return candidate.bundle_qty < incumbent.bundle_qty;
}

}  // namespace trading_engine::order_decision
