#include "engine/signal/edge/TheoreticalEdgeCalculator.h"

#include <algorithm>
#include <limits>

namespace trading_engine::signal {

namespace {

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

[[nodiscard]] std::int64_t edge_bps(
    std::int64_t unit_edge_tick,
    std::int64_t cost_per_bundle_tick
) noexcept {
    if (cost_per_bundle_tick == 0) {
        if (unit_edge_tick > 0) {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (unit_edge_tick < 0) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return 0;
    }

    const auto value =
        static_cast<__int128>(unit_edge_tick) * 10'000 /
        static_cast<__int128>(cost_per_bundle_tick);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value < std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

}  // namespace

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

    out.unit_edge_tick =
        out.guaranteed_payout_per_bundle_tick -
        out.vwap_cost_per_bundle_tick -
        out.fee_per_bundle_tick -
        out.latency_buffer_per_bundle_tick -
        out.slippage_buffer_per_bundle_tick;

    if (!checked_mul_i64(out.unit_edge_tick, out.bundle_qty, &out.total_edge_tick)) {
        out.failure_reason = EdgeFailureReason::InvalidCost;
        return out;
    }

    out.edge_bps = edge_bps(
        out.unit_edge_tick,
        out.vwap_cost_per_bundle_tick
    );

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
