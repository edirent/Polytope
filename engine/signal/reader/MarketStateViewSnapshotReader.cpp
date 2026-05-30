#include "engine/signal/reader/MarketStateViewSnapshotReader.h"

#include <unordered_set>

namespace trading_engine::signal {

MarketStateViewSnapshotReader::MarketStateViewSnapshotReader(
    const trading_engine::state::MarketStateView& view
) noexcept
    : view_(view) {}

SnapshotReadResult MarketStateViewSnapshotReader::read_for_bundle(
    const CandidateBundle& bundle,
    const SignalConfig& config,
    std::uint64_t now_ns
) const {
    std::vector<MarketStateSnapshot> snapshots;
    snapshots.reserve(bundle.leg_count);
    std::unordered_set<std::string> seen_assets;
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& leg = bundle.legs[i];
        if (!seen_assets.insert(leg.asset_id).second) {
            continue;
        }
        const auto snapshot = view_.get_snapshot(leg.asset_id);
        if (snapshot.ok) {
            snapshots.push_back(snapshot.value);
        }
    }

    return validate_bundle_snapshots(bundle, config, snapshots, now_ns);
}

}  // namespace trading_engine::signal
