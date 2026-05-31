#pragma once

#include "engine/risk/public/RiskTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::risk {

inline constexpr std::uint16_t kMaxRevalidatedLegCosts = 16;

struct RevalidatedLegCost {
    std::string asset_id;
    std::int64_t requested_qty_lots = 0;
    std::int64_t executable_qty_lots = 0;
    std::int64_t depth_margin_bps = 0;
    bool enough_depth = false;
};

struct CostRevalidationResult {
    bool ok = false;

    std::int64_t risk_total_cost_tick = 0;
    std::int64_t risk_bundle_qty = 0;
    std::int64_t fee_tick = 0;
    std::int64_t slippage_buffer_tick = 0;
    std::int64_t latency_buffer_tick = 0;
    std::int64_t max_leg_slippage_tick = 0;

    std::int64_t cost_drift_tick = 0;
    RiskVWAPMode vwap_mode = RiskVWAPMode::RecomputedFromSnapshot;

    std::uint16_t leg_count = 0;
    std::array<RevalidatedLegCost, kMaxRevalidatedLegCosts> legs{};

    RiskDecisionType rejection = RiskDecisionType::Approve;
    std::string reason;
};

}  // namespace trading_engine::risk
