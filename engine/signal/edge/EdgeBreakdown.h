#pragma once

#include <cstdint>

namespace trading_engine::signal {

enum class EdgeFailureReason : std::uint8_t {
    None,
    InvalidCost,
    BelowMinUnitEdge,
    BelowMinTotalEdge,
    BelowMinEdgeBps,
    BelowMinBundleQty
};

struct EdgeBreakdown {
    bool passed = false;

    std::int64_t guaranteed_payout_per_bundle_tick = 0;
    std::int64_t vwap_cost_per_bundle_tick = 0;

    std::int64_t fee_per_bundle_tick = 0;
    std::int64_t latency_buffer_per_bundle_tick = 0;
    std::int64_t slippage_buffer_per_bundle_tick = 0;

    std::int64_t unit_edge_tick = 0;
    std::int64_t total_edge_tick = 0;
    std::int64_t edge_bps = 0;

    std::int64_t bundle_qty = 0;

    EdgeFailureReason failure_reason = EdgeFailureReason::None;
};

}  // namespace trading_engine::signal
