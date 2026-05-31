#include "engine/signal/publish/CapturingIntentPublisher.h"

#include <algorithm>

namespace trading_engine::signal {

SignalEvidenceView CapturedSignalEvidence::view() const noexcept {
    SignalEvidenceView out;
    out.snapshots = snapshot_count == 0 ? nullptr : snapshots.data();
    out.snapshot_count = snapshot_count;
    out.depth_views = depth_view_count == 0 ? nullptr : depth_views.data();
    out.depth_view_count = depth_view_count;
    out.snapshot_version_hash = snapshot_version_hash;
    out.read_ts_ns = read_ts_ns;
    return out;
}

void CapturingIntentPublisher::publish(const OpportunityIntent& intent) {
    intents_.push_back(intent);
    evidence_.push_back({});
}

void CapturingIntentPublisher::publish(
    const OpportunityIntent& intent,
    const SignalEvidenceView& evidence
) {
    intents_.push_back(intent);

    CapturedSignalEvidence captured;
    captured.snapshot_count = std::min<std::uint16_t>(
        evidence.snapshot_count,
        kMaxSignalEvidenceSnapshots
    );
    for (std::uint16_t i = 0;
         i < captured.snapshot_count && evidence.snapshots != nullptr;
         ++i) {
        captured.snapshots[i] = evidence.snapshots[i];
    }
    captured.depth_view_count = std::min<std::uint16_t>(
        evidence.depth_view_count,
        kMaxSignalEvidenceSnapshots
    );
    for (std::uint16_t i = 0;
         i < captured.depth_view_count && evidence.depth_views != nullptr;
         ++i) {
        captured.depth_views[i] = evidence.depth_views[i];
    }
    captured.snapshot_version_hash = evidence.snapshot_version_hash;
    captured.read_ts_ns = evidence.read_ts_ns;
    evidence_.push_back(std::move(captured));
}

const std::vector<OpportunityIntent>& CapturingIntentPublisher::intents()
    const noexcept {
    return intents_;
}

SignalEvidenceView CapturingIntentPublisher::evidence_at(
    std::size_t index
) const noexcept {
    if (index >= evidence_.size()) {
        return {};
    }
    return evidence_[index].view();
}

}  // namespace trading_engine::signal
