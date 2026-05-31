#pragma once

#include "state/book/DepthPrefix.h"
#include "state/MarketStateSnapshot.h"
#include "state/StateTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace trading_engine::state {

constexpr std::uint16_t kMaxDepthBatchViews = 16;

struct MarketDepthView {
    std::uint32_t asset_index = 0;

    std::uint64_t book_version = 0;
    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t last_ws_recv_ns = 0;

    bool usable_for_depth = false;
    bool recovering = false;
    bool crossed = false;
    bool closed = false;
    bool resolved = false;

    std::uint16_t bid_count = 0;
    std::uint16_t ask_count = 0;

    std::array<PriceLevel, kMaxSnapshotDepth> bids{};
    std::array<PriceLevel, kMaxSnapshotDepth> asks{};

    DepthPrefix prefix{};
};

struct DepthBatchReadResult {
    bool ok = false;
    std::uint16_t count = 0;
    std::array<MarketDepthView, kMaxDepthBatchViews> views{};
    std::uint64_t combined_snapshot_hash = 0;
};

[[nodiscard]] inline MarketDepthView market_depth_view_from_snapshot(
    const MarketStateSnapshot& snapshot,
    std::uint32_t asset_index = 0
) {
    MarketDepthView view;
    view.asset_index = asset_index;
    view.book_version = snapshot.version;
    view.snapshot_version_hash = snapshot.snapshot_version_hash != 0
        ? snapshot.snapshot_version_hash
        : snapshot.state_hash;
    view.last_ws_recv_ns = snapshot.last_book_update_ns;
    view.usable_for_depth = snapshot.usable_for_depth;
    view.recovering = snapshot.recovering;
    view.crossed = snapshot.crossed;
    view.closed = snapshot.closed;
    view.resolved = snapshot.resolved;

    view.bid_count = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(snapshot.bid_count, kMaxSnapshotDepth)
    );
    view.ask_count = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(snapshot.ask_count, kMaxSnapshotDepth)
    );
    for (std::uint16_t i = 0; i < view.bid_count; ++i) {
        view.bids[i] = snapshot.bids[i];
    }
    for (std::uint16_t i = 0; i < view.ask_count; ++i) {
        view.asks[i] = snapshot.asks[i];
    }
    build_depth_prefix(
        view.bids,
        view.bid_count,
        view.asks,
        view.ask_count,
        &view.prefix
    );
    return view;
}

[[nodiscard]] inline PrefixVwapResult buy_vwap_from_prefix(
    const MarketDepthView& view,
    std::int64_t qty_lots
) noexcept {
    return buy_vwap_from_prefix(view.asks, view.prefix, qty_lots);
}

[[nodiscard]] inline std::uint64_t hash_depth_views(
    std::span<const MarketDepthView> views
) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
            hash *= 1099511628211ULL;
        }
    };

    for (const auto& view : views) {
        mix(view.asset_index);
        mix(view.book_version);
        mix(view.snapshot_version_hash);
        mix(view.last_ws_recv_ns);
    }
    return hash;
}

}  // namespace trading_engine::state
