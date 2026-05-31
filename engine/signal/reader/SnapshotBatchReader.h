#pragma once

#include "engine/signal/reader/MarketSnapshotReader.h"
#include "engine/signal/scan/BundleRuntimePlan.h"

#include <array>
#include <cstdint>

namespace trading_engine::signal {

struct SnapshotBatchReadResult {
    bool ok = false;

    IntentStatus rejection_status = IntentStatus::CandidateOnly;
    std::string error;

    SnapshotVersion snapshot_version;
    std::uint64_t snapshot_consistency_guard_ns = 0;

    std::uint16_t snapshot_count = 0;
    std::array<MarketStateSnapshot, kMaxIntentLegs> snapshots{};
};

class ISnapshotBatchReader {
public:
    [[nodiscard]] virtual SnapshotBatchReadResult read_for_plan(
        const BundleRuntimePlan& plan,
        const SignalConfig& config,
        std::uint64_t now_ns = 0
    ) const = 0;

    virtual ~ISnapshotBatchReader() = default;
};

[[nodiscard]] SnapshotBatchReadResult validate_plan_snapshots(
    const BundleRuntimePlan& plan,
    const SignalConfig& config,
    const std::array<MarketStateSnapshot, kMaxIntentLegs>& snapshots,
    std::uint16_t snapshot_count,
    std::uint64_t now_ns = 0
);

}  // namespace trading_engine::signal
