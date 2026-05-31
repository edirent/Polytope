#pragma once

#include "engine/signal/pricing/SideResolver.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "oracle/public/CandidateBundle.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::signal {

struct BundleRuntimePlan {
    const trading_engine::oracle::CandidateBundle* bundle = nullptr;

    std::uint64_t bundle_id = 0;
    std::uint64_t bundle_hash = 0;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t constraint_hash = 0;

    std::uint16_t leg_count = 0;
    std::uint16_t unique_asset_count = 0;

    std::array<const std::string*, kMaxIntentLegs> market_ids{};
    std::array<const std::string*, kMaxIntentLegs> asset_ids{};
    std::array<std::uint32_t, kMaxIntentLegs> asset_indices{};
    std::array<Side, kMaxIntentLegs> sides{};
    std::array<ExecutableBookSide, kMaxIntentLegs> executable_sides{};
    std::array<std::int64_t, kMaxIntentLegs> ratio_qty_lots{};
    std::array<std::int64_t, kMaxIntentLegs> max_price_ticks{};

    std::array<const std::string*, kMaxIntentLegs> unique_asset_ids{};
    std::array<std::uint32_t, kMaxIntentLegs> unique_asset_indices{};

    std::int64_t guaranteed_payout_tick = 0;
    std::int64_t min_unit_edge_tick = 0;
    std::int64_t min_total_edge_tick = 0;
    std::int64_t min_edge_bps = 0;
};

}  // namespace trading_engine::signal
