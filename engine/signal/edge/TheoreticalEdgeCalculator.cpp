#include "engine/signal/edge/TheoreticalEdgeCalculator.h"

namespace trading_engine::signal {

TheoreticalEdgeCalculator::TheoreticalEdgeCalculator(
    FeeModel fee_model,
    LatencyBufferModel latency_buffer_model
) : fee_model_(fee_model),
    latency_buffer_model_(latency_buffer_model) {}

EdgeBreakdown TheoreticalEdgeCalculator::calculate(
    const CandidateBundle& bundle,
    const CostResult& cost
) const noexcept {
    EdgeBreakdown out;
    out.guaranteed_payout_tick = bundle.guaranteed_payout_tick;
    out.total_cost_tick = cost.total_cost_tick;
    out.fee_tick = fee_model_.estimate_fee_tick(bundle, cost);
    out.latency_buffer_tick =
        latency_buffer_model_.estimate_latency_buffer_tick(bundle);
    out.estimated_edge_tick =
        out.guaranteed_payout_tick -
        out.total_cost_tick -
        out.fee_tick -
        out.latency_buffer_tick;
    out.min_edge_tick = bundle.min_edge_tick;
    out.above_threshold = out.estimated_edge_tick >= out.min_edge_tick;
    return out;
}

}  // namespace trading_engine::signal
