#pragma once

#include "state/snapshot/PublishedMarketSnapshot.h"
#include "state/view/MarketDepthView.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace trading_engine::state {

struct SnapshotSlot {
    PublishedMarketSnapshot snapshot;
    MarketDepthView depth_view;
};

struct SnapshotBuffer {
    std::array<SnapshotSlot, 2> slots{};
    std::atomic<std::uint8_t> active_index{0};
    std::atomic<bool> published{false};
};

}  // namespace trading_engine::state
