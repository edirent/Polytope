#pragma once

#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/StateTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::risk {

inline constexpr std::uint16_t kMaxRiskInputSnapshots = 16;

struct MarketDepthView {
    std::string market_id;
    std::string asset_id;

    std::uint64_t snapshot_version = 0;
    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t last_book_update_ns = 0;

    bool usable_for_depth = false;
    bool recovering = false;
    bool crossed = false;
    bool closed = false;
    bool resolved = false;

    std::uint32_t bid_count = 0;
    std::uint32_t ask_count = 0;

    std::array<state::PriceLevel, state::kMaxSnapshotDepth> bids{};
    std::array<state::PriceLevel, state::kMaxSnapshotDepth> asks{};
};

struct RiskInputEnvelope {
    signal::OpportunityIntent intent;

    std::uint16_t snapshot_count = 0;
    std::array<MarketDepthView, kMaxRiskInputSnapshots> depth_views{};

    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t now_ns = 0;

    std::uint64_t policy_version = 0;
    std::uint64_t policy_hash = 0;
};

}  // namespace trading_engine::risk
