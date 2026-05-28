#pragma once

#include <cstdint>

namespace trading_engine::signal {

struct EdgeBreakdown {
    std::int64_t guaranteed_payout_tick = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t fee_tick = 0;
    std::int64_t latency_buffer_tick = 0;
    std::int64_t estimated_edge_tick = 0;

    std::int64_t min_edge_tick = 0;
    bool above_threshold = false;
};

}  // namespace trading_engine::signal
