#pragma once

#include "engine/risk/public/RiskTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::risk {

struct CostRevalidationResult {
    bool ok = false;

    std::int64_t risk_total_cost_tick = 0;
    std::int64_t risk_bundle_qty = 0;
    std::int64_t fee_tick = 0;
    std::int64_t slippage_buffer_tick = 0;
    std::int64_t latency_buffer_tick = 0;

    std::int64_t cost_drift_tick = 0;

    RiskDecisionType rejection = RiskDecisionType::Approve;
    std::string reason;
};

}  // namespace trading_engine::risk
