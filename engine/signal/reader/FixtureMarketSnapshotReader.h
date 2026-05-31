#pragma once

#include "engine/signal/reader/MarketSnapshotReader.h"
#include "engine/signal/reader/SnapshotBatchReader.h"

#include <filesystem>
#include <unordered_map>

namespace trading_engine::signal {

class FixtureMarketSnapshotReader final
    : public IMarketSnapshotReader,
      public ISnapshotBatchReader,
      public IDepthBatchReader {
public:
    [[nodiscard]] bool load(
        const std::filesystem::path& path,
        std::string* error = nullptr
    );

    [[nodiscard]] SnapshotReadResult read_for_bundle(
        const CandidateBundle& bundle,
        const SignalConfig& config,
        std::uint64_t now_ns = 0
    ) const override;

    [[nodiscard]] SnapshotBatchReadResult read_for_plan(
        const BundleRuntimePlan& plan,
        const SignalConfig& config,
        std::uint64_t now_ns = 0
    ) const override;

    [[nodiscard]] DepthReadResult read_depth_for_plan(
        const BundleRuntimePlan& plan,
        const SignalConfig& config,
        std::uint64_t now_ns = 0
    ) const override;

private:
    std::unordered_map<std::string, MarketStateSnapshot> snapshots_;
};

}  // namespace trading_engine::signal
