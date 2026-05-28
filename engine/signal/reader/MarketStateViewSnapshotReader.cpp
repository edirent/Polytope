#include "engine/signal/reader/MarketStateViewSnapshotReader.h"

namespace trading_engine::signal {

MarketStateViewSnapshotReader::MarketStateViewSnapshotReader(
    const trading_engine::state::MarketStateView& view
) noexcept
    : view_(view) {}

SnapshotReadResult MarketStateViewSnapshotReader::read_for_bundle(
    const CandidateBundle& bundle,
    const SignalConfig& config
) const {
    std::vector<MarketStateSnapshot> snapshots;
    snapshots.reserve(bundle.leg_count);
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& leg = bundle.legs[i];
        const auto snapshot = view_.get_snapshot(leg.asset_id);
        if (snapshot.ok) {
            snapshots.push_back(snapshot.value);
        }
    }

    return validate_bundle_snapshots(bundle, config, snapshots);
}

}  // namespace trading_engine::signal
