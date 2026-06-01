#pragma once

#include <cstdint>

namespace trading_engine::order_decision {

struct EdgeCurvePoint {
    std::int64_t bundle_qty = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t unit_edge_tick = 0;
    std::int64_t total_edge_tick = 0;
    std::int64_t edge_bps = 0;
};

}  // namespace trading_engine::order_decision
