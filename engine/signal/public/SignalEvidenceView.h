#pragma once

#include "state/MarketStateSnapshot.h"
#include "state/view/MarketDepthView.h"

#include <cstdint>

namespace trading_engine::signal {

struct SignalEvidenceView {
    const trading_engine::state::MarketStateSnapshot* snapshots = nullptr;
    std::uint16_t snapshot_count = 0;

    const trading_engine::state::MarketDepthView* depth_views = nullptr;
    std::uint16_t depth_view_count = 0;

    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t read_ts_ns = 0;
};

}  // namespace trading_engine::signal
