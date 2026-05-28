#pragma once

#include "engine/signal/public/OpportunityIntent.h"

#include <array>
#include <cstdint>

namespace trading_engine::signal {

enum class CostFailureReason : std::uint8_t {
    None,
    MissingSnapshot,
    MissingBookSide,
    InsufficientDepth,
    InvalidQuantity,
    InvalidLeg
};

struct CostResult {
    bool enough_depth = false;

    std::int64_t total_cost_tick = 0;
    std::int64_t worst_price_tick = 0;
    std::int64_t bundle_vwap_tick = 0;

    std::uint16_t filled_leg_count = 0;
    std::uint16_t failed_leg_index = 0;

    CostFailureReason failure_reason = CostFailureReason::None;

    std::array<IntentLeg, kMaxIntentLegs> priced_legs{};
};

}  // namespace trading_engine::signal
