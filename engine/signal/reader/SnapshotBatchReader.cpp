#include "engine/signal/reader/SnapshotBatchReader.h"

#include "engine/signal/reader/SnapshotConsistencyGuard.h"

#include <algorithm>
#include <chrono>
#include <span>
#include <string>

namespace trading_engine::signal {
namespace {

[[nodiscard]] std::uint64_t elapsed_ns(
    std::chrono::steady_clock::time_point start
) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start
        ).count()
    );
}

[[nodiscard]] SnapshotBatchReadResult reject(
    IntentStatus status,
    std::string error,
    SnapshotVersion version = {},
    std::uint64_t guard_ns = 0
) {
    SnapshotBatchReadResult result;
    result.rejection_status = status;
    result.error = std::move(error);
    result.snapshot_version = version;
    result.snapshot_consistency_guard_ns = guard_ns;
    return result;
}

[[nodiscard]] DepthReadResult reject_depth(
    IntentStatus status,
    std::string error,
    SnapshotVersion version = {},
    std::uint64_t guard_ns = 0
) {
    DepthReadResult result;
    result.rejection_status = status;
    result.error = std::move(error);
    result.snapshot_version = version;
    result.snapshot_version_hash = version.combined_hash;
    result.snapshot_consistency_guard_ns = guard_ns;
    return result;
}

[[nodiscard]] bool has_snapshot_for_asset(
    std::span<const MarketStateSnapshot> snapshots,
    const std::string& asset_id
) {
    for (const auto& snapshot : snapshots) {
        if (snapshot.entity_id == asset_id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_depth_for_asset_index(
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    std::uint32_t asset_index
) {
    for (const auto& view : depth_views) {
        if (view.asset_index == asset_index) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint64_t guard_now_ns(
    std::span<const MarketStateSnapshot> snapshots,
    std::uint64_t requested_now_ns
) noexcept {
    if (requested_now_ns != 0) {
        return requested_now_ns;
    }

    std::uint64_t now_ns = 0;
    for (const auto& snapshot : snapshots) {
        now_ns = std::max(now_ns, snapshot.last_book_update_ns);
    }
    return now_ns;
}

[[nodiscard]] std::uint64_t guard_now_ns(
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    std::uint64_t requested_now_ns
) noexcept {
    if (requested_now_ns != 0) {
        return requested_now_ns;
    }

    std::uint64_t now_ns = 0;
    for (const auto& view : depth_views) {
        now_ns = std::max(now_ns, view.last_ws_recv_ns);
    }
    return now_ns;
}

}  // namespace

SnapshotBatchReadResult validate_plan_snapshots(
    const BundleRuntimePlan& plan,
    const SignalConfig& config,
    const std::array<MarketStateSnapshot, kMaxIntentLegs>& snapshots,
    std::uint16_t snapshot_count,
    std::uint64_t now_ns
) {
    const auto snapshot_span = std::span<const MarketStateSnapshot>(
        snapshots.data(),
        snapshot_count
    );

    if (snapshot_count < plan.unique_asset_count) {
        return reject(
            IntentStatus::RejectedMissingSnapshot,
            "missing bundle snapshot"
        );
    }

    for (std::uint16_t i = 0; i < plan.unique_asset_count; ++i) {
        const auto* asset_id = plan.unique_asset_ids[i];
        if (!asset_id || !has_snapshot_for_asset(snapshot_span, *asset_id)) {
            return reject(
                IntentStatus::RejectedMissingSnapshot,
                asset_id ? "missing snapshot for asset: " + *asset_id
                         : "missing snapshot for asset"
            );
        }
    }

    SnapshotConsistencyGuard guard;
    const auto guard_start = std::chrono::steady_clock::now();
    const auto guard_result = guard.check(
        snapshot_span,
        guard_now_ns(snapshot_span, now_ns),
        config
    );
    const auto guard_ns = elapsed_ns(guard_start);
    if (!guard_result.ok) {
        return reject(
            guard_result.rejection_status,
            guard_result.error,
            guard_result.version,
            guard_ns
        );
    }

    SnapshotBatchReadResult result;
    result.ok = true;
    result.snapshot_count = snapshot_count;
    result.snapshots = snapshots;
    result.snapshot_version = guard_result.version;
    result.snapshot_consistency_guard_ns = guard_ns;
    return result;
}

DepthReadResult validate_plan_depth_views(
    const BundleRuntimePlan& plan,
    const SignalConfig& config,
    const std::array<trading_engine::state::MarketDepthView, kMaxIntentLegs>&
        depth_views,
    std::uint16_t depth_count,
    std::uint64_t now_ns
) {
    const auto depth_span =
        std::span<const trading_engine::state::MarketDepthView>(
            depth_views.data(),
            depth_count
        );

    if (depth_count < plan.unique_asset_count) {
        return reject_depth(
            IntentStatus::RejectedMissingSnapshot,
            "missing bundle depth view"
        );
    }

    for (std::uint16_t i = 0; i < plan.unique_asset_count; ++i) {
        if (!has_depth_for_asset_index(
                depth_span,
                plan.unique_asset_indices[i]
            )) {
            return reject_depth(
                IntentStatus::RejectedMissingSnapshot,
                "missing depth view for asset"
            );
        }
    }

    SnapshotConsistencyGuard guard;
    const auto guard_start = std::chrono::steady_clock::now();
    const auto guard_result = guard.check(
        depth_span,
        guard_now_ns(depth_span, now_ns),
        config
    );
    const auto guard_ns = elapsed_ns(guard_start);
    if (!guard_result.ok) {
        return reject_depth(
            guard_result.rejection_status,
            guard_result.error,
            guard_result.version,
            guard_ns
        );
    }

    DepthReadResult result;
    result.ok = true;
    result.count = depth_count;
    result.depth_views = depth_views;
    result.snapshot_version = guard_result.version;
    result.snapshot_version_hash = guard_result.version.combined_hash;
    result.snapshot_consistency_guard_ns = guard_ns;
    return result;
}

}  // namespace trading_engine::signal
