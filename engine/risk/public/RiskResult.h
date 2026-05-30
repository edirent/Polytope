#pragma once

#include "engine/risk/metrics/RiskMetrics.h"

#include <cstdint>

namespace trading_engine::risk {

struct RiskResult {
    std::uint64_t intents_evaluated = 0;
    std::uint64_t intents_approved = 0;
    std::uint64_t intents_rejected = 0;

    std::uint64_t rejected_kill_switch = 0;
    std::uint64_t rejected_low_edge = 0;
    std::uint64_t rejected_cost_limit = 0;
    std::uint64_t rejected_exposure_limit = 0;
    std::uint64_t rejected_inventory_limit = 0;
    std::uint64_t rejected_stale_or_expired = 0;
    std::uint64_t rejected_drift_or_slippage = 0;
    std::uint64_t rejected_pending_or_rate_limit = 0;
    std::uint64_t rejected_bad_market_state = 0;
    std::uint64_t rejected_snapshot_freshness = 0;
    std::uint64_t rejected_partial_fill_risk = 0;

    std::uint64_t output_hash = 0;
    RiskMetrics metrics;
};

}  // namespace trading_engine::risk
