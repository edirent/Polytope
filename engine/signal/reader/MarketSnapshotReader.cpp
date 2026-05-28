#include "engine/signal/reader/MarketSnapshotReader.h"

#include "state/quality/BookQualityState.h"

#include <unordered_set>

namespace trading_engine::signal {

namespace {

[[nodiscard]] SnapshotReadResult reject(
    IntentStatus status,
    std::string error
) {
    SnapshotReadResult result;
    result.ok = false;
    result.rejection_status = status;
    result.error = std::move(error);
    return result;
}

[[nodiscard]] bool bad_quality(
    const MarketStateSnapshot& snapshot
) noexcept {
    using trading_engine::state::BookQuality;
    return snapshot.quality == BookQuality::Recovering ||
           snapshot.quality == BookQuality::Crossed ||
           snapshot.quality == BookQuality::Stale ||
           snapshot.quality == BookQuality::Closed ||
           snapshot.quality == BookQuality::Resolved;
}

}  // namespace

SnapshotReadResult validate_bundle_snapshots(
    const CandidateBundle& bundle,
    const SignalConfig& config,
    const std::vector<MarketStateSnapshot>& snapshots
) {
    std::unordered_set<std::string> seen_assets;
    for (const auto& snapshot : snapshots) {
        seen_assets.insert(snapshot.entity_id);
    }

    if (snapshots.size() < bundle.leg_count) {
        return reject(
            IntentStatus::RejectedMissingSnapshot,
            "missing bundle snapshot"
        );
    }

    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& leg = bundle.legs[i];
        if (!seen_assets.contains(leg.asset_id)) {
            return reject(
                IntentStatus::RejectedMissingSnapshot,
                "missing snapshot for asset: " + leg.asset_id
            );
        }
    }

    for (const auto& snapshot : snapshots) {
        if (snapshot.recovering) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot recovering: " + snapshot.entity_id
            );
        }
        if (snapshot.crossed) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot crossed: " + snapshot.entity_id
            );
        }
        if (snapshot.closed) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot closed: " + snapshot.entity_id
            );
        }
        if (snapshot.resolved) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot resolved: " + snapshot.entity_id
            );
        }
        if (bad_quality(snapshot)) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "bad snapshot quality: " + snapshot.entity_id
            );
        }
        if (config.require_usable_for_depth && !snapshot.usable_for_depth) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot not usable for depth: " + snapshot.entity_id
            );
        }
        if (config.require_usable_for_signal && !snapshot.usable_for_signal) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot not usable for signal: " + snapshot.entity_id
            );
        }
    }

    SnapshotReadResult result;
    result.ok = true;
    result.snapshots = snapshots;
    return result;
}

}  // namespace trading_engine::signal
