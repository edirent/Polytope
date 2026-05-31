#pragma once

#include "engine/signal/public/OpportunityIntent.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::signal {

enum class CostFailureReason : std::uint8_t {
    None,
    MissingSnapshot,
    MissingBookSide,
    InsufficientDepth,
    InvalidQuantity,
    InvalidLeg
};

struct FillSimulationLeg {
    std::string asset_id;

    std::int64_t requested_qty_lots = 0;
    std::int64_t executable_qty_lots = 0;

    std::int64_t vwap_price_tick = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t worst_price_tick = 0;

    std::uint64_t book_age_ns = 0;

    bool enough_depth = false;
};

struct CostResult {
    bool executable = false;

    std::int64_t bundle_qty = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t avg_cost_tick = 0;
    std::int64_t max_leg_slippage_tick = 0;

    CostFailureReason failure_reason = CostFailureReason::None;

    std::uint64_t price_vector_builder_ns = 0;
    std::uint64_t vwap_precheck_ns = 0;

    std::uint16_t fixed_leg_count = 0;
    std::array<FillSimulationLeg, kMaxIntentLegs> fixed_legs{};

    std::vector<FillSimulationLeg> legs;
};

[[nodiscard]] inline std::uint16_t cost_leg_count(
    const CostResult& cost
) noexcept {
    if (cost.fixed_leg_count != 0) {
        return cost.fixed_leg_count;
    }
    return static_cast<std::uint16_t>(
        std::min<std::size_t>(cost.legs.size(), kMaxIntentLegs)
    );
}

[[nodiscard]] inline const FillSimulationLeg& cost_leg_at(
    const CostResult& cost,
    std::uint16_t index
) noexcept {
    return cost.fixed_leg_count != 0 ? cost.fixed_legs[index] :
                                      cost.legs[index];
}

}  // namespace trading_engine::signal
