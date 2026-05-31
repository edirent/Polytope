#pragma once

#include "engine/signal/reader/MarketSnapshotReader.h"
#include "engine/signal/scan/BundleRuntimePlan.h"
#include "state/view/MarketDepthView.h"

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

struct DepthReadResult {
    bool ok = false;

    IntentStatus rejection_status = IntentStatus::CandidateOnly;
    std::string error;

    SnapshotVersion snapshot_version;
    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t snapshot_consistency_guard_ns = 0;

    std::uint16_t count = 0;
    std::array<trading_engine::state::MarketDepthView, kMaxIntentLegs>
        depth_views{};
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

class IDepthBatchReader {
public:
    [[nodiscard]] virtual DepthReadResult read_depth_for_plan(
        const BundleRuntimePlan& plan,
        const SignalConfig& config,
        std::uint64_t now_ns = 0
    ) const = 0;

    virtual ~IDepthBatchReader() = default;
};

[[nodiscard]] SnapshotBatchReadResult validate_plan_snapshots(
    const BundleRuntimePlan& plan,
    const SignalConfig& config,
    const std::array<MarketStateSnapshot, kMaxIntentLegs>& snapshots,
    std::uint16_t snapshot_count,
    std::uint64_t now_ns = 0
);

[[nodiscard]] DepthReadResult validate_plan_depth_views(
    const BundleRuntimePlan& plan,
    const SignalConfig& config,
    const std::array<trading_engine::state::MarketDepthView, kMaxIntentLegs>&
        depth_views,
    std::uint16_t depth_count,
    std::uint64_t now_ns = 0
);

}  // namespace trading_engine::signal
