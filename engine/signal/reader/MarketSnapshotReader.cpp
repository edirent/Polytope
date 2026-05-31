#include "engine/signal/reader/MarketSnapshotReader.h"

#include "engine/signal/reader/SnapshotConsistencyGuard.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_set>
#include <vector>

namespace trading_engine::signal {

namespace {

[[nodiscard]] SnapshotReadResult reject(
    IntentStatus status,
    std::string error,
    SnapshotVersion version = {},
    std::uint64_t guard_ns = 0
) {
    SnapshotReadResult result;
    result.ok = false;
    result.rejection_status = status;
    result.error = std::move(error);
    result.snapshot_version = version;
    result.snapshot_consistency_guard_ns = guard_ns;
    return result;
}

[[nodiscard]] std::uint64_t elapsed_ns(
    std::chrono::steady_clock::time_point start
) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start
        ).count()
    );
}

std::vector<std::string> unique_bundle_assets(const CandidateBundle& bundle) {
    std::vector<std::string> assets;
    std::unordered_set<std::string> seen;
    assets.reserve(bundle.leg_count);

    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& asset_id = bundle.legs[i].asset_id;
        if (asset_id.empty()) {
            continue;
        }
        if (seen.insert(asset_id).second) {
            assets.push_back(asset_id);
        }
    }

    return assets;
}

[[nodiscard]] std::uint64_t guard_now_ns(
    const std::vector<MarketStateSnapshot>& snapshots,
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

}  // namespace

SnapshotReadResult validate_bundle_snapshots(
    const CandidateBundle& bundle,
    const SignalConfig& config,
    const std::vector<MarketStateSnapshot>& snapshots,
    std::uint64_t now_ns
) {
    const auto required_assets = unique_bundle_assets(bundle);
    std::unordered_set<std::string> seen_assets;
    for (const auto& snapshot : snapshots) {
        seen_assets.insert(snapshot.entity_id);
    }

    if (snapshots.size() < required_assets.size()) {
        return reject(
            IntentStatus::RejectedMissingSnapshot,
            "missing bundle snapshot"
        );
    }

    for (const auto& asset_id : required_assets) {
        if (!seen_assets.contains(asset_id)) {
            return reject(
                IntentStatus::RejectedMissingSnapshot,
                "missing snapshot for asset: " + asset_id
            );
        }
    }

    SnapshotConsistencyGuard guard;
    const auto guard_start = std::chrono::steady_clock::now();
    const auto guard_result = guard.check(
        snapshots,
        guard_now_ns(snapshots, now_ns),
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

    SnapshotReadResult result;
    result.ok = true;
    result.snapshots = snapshots;
    result.snapshot_version = guard_result.version;
    result.snapshot_consistency_guard_ns = guard_ns;
    return result;
}

}  // namespace trading_engine::signal
