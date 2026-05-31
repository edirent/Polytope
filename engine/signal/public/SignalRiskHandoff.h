#pragma once

#include "engine/signal/public/OpportunityIntent.h"
#include "engine/signal/public/SignalEvidenceView.h"

#include <cstdint>

namespace trading_engine::signal {

struct SignalRiskHandoff {
    const OpportunityIntent* intent = nullptr;

    const trading_engine::state::MarketStateSnapshot* snapshots = nullptr;
    std::uint16_t snapshot_count = 0;

    const trading_engine::state::MarketDepthView* depth_views = nullptr;
    std::uint16_t depth_view_count = 0;

    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t now_ns = 0;
};

[[nodiscard]] inline SignalRiskHandoff make_signal_risk_handoff(
    const OpportunityIntent& intent,
    const SignalEvidenceView& evidence,
    std::uint64_t now_ns
) noexcept {
    SignalRiskHandoff out;
    out.intent = &intent;
    out.snapshots = evidence.snapshots;
    out.snapshot_count = evidence.snapshot_count;
    out.depth_views = evidence.depth_views;
    out.depth_view_count = evidence.depth_view_count;
    out.snapshot_version_hash = evidence.snapshot_version_hash;
    out.now_ns = now_ns;
    return out;
}

}  // namespace trading_engine::signal
