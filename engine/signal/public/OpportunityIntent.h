#pragma once

#include "engine/signal/public/SignalTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::signal {

inline constexpr std::uint16_t kMaxIntentLegs = 16;

struct IntentLeg {
    std::string market_id;
    std::string asset_id;

    Side side = Side::Buy;

    std::int64_t quantity_lots = 0;

    std::int64_t estimated_vwap_tick = 0;
    std::int64_t worst_price_tick = 0;
    std::int64_t estimated_cost_tick = 0;

    bool enough_depth = false;
};

struct OpportunityIntent {
    std::uint64_t intent_id = 0;
    std::uint64_t bundle_id = 0;

    IntentStatus status = IntentStatus::CandidateOnly;

    bool valid_under_settlement = false;
    bool passed_quality_gate = false;
    bool enough_depth = false;

    std::int64_t guaranteed_payout_tick = 0;
    std::int64_t estimated_cost_tick = 0;
    std::int64_t estimated_fee_tick = 0;
    std::int64_t latency_buffer_tick = 0;
    std::int64_t estimated_edge_tick = 0;
    std::int64_t min_edge_tick = 0;

    std::uint16_t leg_count = 0;
    std::array<IntentLeg, kMaxIntentLegs> legs{};

    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t oracle_artifact_version = 0;

    std::string reject_reason;
};

}  // namespace trading_engine::signal
