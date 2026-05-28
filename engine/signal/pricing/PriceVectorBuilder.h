#pragma once

#include "engine/signal/pricing/CostResult.h"
#include "engine/signal/reader/MarketSnapshotReader.h"

#include <array>
#include <cstdint>
#include <vector>

namespace trading_engine::signal {

struct PriceVectorResult {
    bool ok = false;
    CostFailureReason failure_reason = CostFailureReason::None;
    std::uint16_t failed_leg_index = 0;
    std::array<const MarketStateSnapshot*, kMaxIntentLegs> snapshots{};
};

[[nodiscard]] PriceVectorResult build_price_vector(
    const CandidateBundle& bundle,
    const std::vector<MarketStateSnapshot>& snapshots
);

}  // namespace trading_engine::signal
