#include "engine/signal/edge/TheoreticalEdgeCalculator.h"

#include "engine/common/math/EdgeMath.h"

#include <algorithm>

namespace trading_engine::signal {

TheoreticalEdgeCalculator::TheoreticalEdgeCalculator(
    FeeModel fee_model,
    LatencyBufferModel latency_buffer_model,
    SlippageBufferModel slippage_buffer_model
) : fee_model_(fee_model),
    latency_buffer_model_(latency_buffer_model),
    slippage_buffer_model_(slippage_buffer_model) {}

EdgeBreakdown TheoreticalEdgeCalculator::calculate(
    const CandidateBundle& bundle,
    const CostResult& cost
) const noexcept {
    return calculate(bundle, cost, SignalConfig{});
}

EdgeBreakdown TheoreticalEdgeCalculator::calculate(
    const CandidateBundle& bundle,
    const CostResult& cost,
    const SignalConfig& config
) const noexcept {
    EdgeBreakdown out;
    out.guaranteed_payout_per_bundle_tick = bundle.guaranteed_payout_tick;
    out.vwap_cost_per_bundle_tick = cost.avg_cost_tick;
    out.fee_per_bundle_tick = fee_model_.estimate_fee_tick(bundle, cost);
    out.latency_buffer_per_bundle_tick =
        latency_buffer_model_.estimate_latency_buffer_tick(bundle);
    out.slippage_buffer_per_bundle_tick =
        slippage_buffer_model_.estimate_slippage_buffer_tick(
            bundle,
            cost,
            config
        );
    out.bundle_qty = cost.bundle_qty;

    if (!cost.executable || cost.bundle_qty <= 0) {
        out.failure_reason = EdgeFailureReason::InvalidCost;
        return out;
    }

    const auto edge = trading_engine::common::math::compute_edge({
        .guaranteed_payout_tick = out.guaranteed_payout_per_bundle_tick,
        .total_cost_tick = out.vwap_cost_per_bundle_tick,
        .fee_tick = out.fee_per_bundle_tick,
        .latency_buffer_tick = out.latency_buffer_per_bundle_tick,
        .slippage_buffer_tick = out.slippage_buffer_per_bundle_tick,
        .bundle_qty = out.bundle_qty
    });
    if (!edge.ok) {
        out.failure_reason = EdgeFailureReason::InvalidCost;
        return out;
    }
    out.unit_edge_tick = edge.unit_edge_tick;
    out.total_edge_tick = edge.total_edge_tick;
    out.edge_bps = edge.edge_bps;

    const auto min_unit_edge_tick = std::max(
        config.min_unit_edge_tick,
        bundle.min_edge_tick
    );
    if (out.bundle_qty < config.min_bundle_qty) {
        out.failure_reason = EdgeFailureReason::BelowMinBundleQty;
        return out;
    }
    if (out.unit_edge_tick < min_unit_edge_tick) {
        out.failure_reason = EdgeFailureReason::BelowMinUnitEdge;
        return out;
    }
    if (out.total_edge_tick < config.min_total_edge_tick) {
        out.failure_reason = EdgeFailureReason::BelowMinTotalEdge;
        return out;
    }
    if (out.edge_bps < config.min_edge_bps) {
        out.failure_reason = EdgeFailureReason::BelowMinEdgeBps;
        return out;
    }

    out.passed = true;
    out.failure_reason = EdgeFailureReason::None;
    return out;
}

}  // namespace trading_engine::signal
