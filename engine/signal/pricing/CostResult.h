#pragma once

#include "engine/signal/public/OpportunityIntent.h"

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

    std::vector<FillSimulationLeg> legs;
};

}  // namespace trading_engine::signal
