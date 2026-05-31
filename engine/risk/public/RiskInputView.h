#pragma once

#include "engine/signal/public/SignalRiskHandoff.h"

#include <cstdint>

namespace trading_engine::signal {
struct OpportunityIntent;
}  // namespace trading_engine::signal

namespace trading_engine::state {
struct MarketStateSnapshot;
}  // namespace trading_engine::state

namespace trading_engine::risk {

struct RiskLedgerSnapshot;
struct RiskPolicySnapshot;

struct RiskInputView {
    const signal::OpportunityIntent* intent = nullptr;

    const state::MarketStateSnapshot* snapshots = nullptr;
    std::uint16_t snapshot_count = 0;

    std::uint64_t snapshot_version_hash = 0;
    // Legacy compatibility only. Hot-path risk evaluation ignores this field.
    std::uint64_t latest_snapshot_version_hash = 0;

    std::uint64_t now_ns = 0;

    const RiskPolicySnapshot* policy = nullptr;
    const RiskLedgerSnapshot* ledger = nullptr;
};

[[nodiscard]] inline RiskInputView make_risk_input_view(
    const signal::SignalRiskHandoff& handoff
) noexcept {
    RiskInputView out;
    out.intent = handoff.intent;
    out.snapshots = handoff.snapshots;
    out.snapshot_count = handoff.snapshot_count;
    out.snapshot_version_hash = handoff.snapshot_version_hash;
    out.now_ns = handoff.now_ns;
    return out;
}

}  // namespace trading_engine::risk
