#pragma once

#include "engine/signal/reader/MarketSnapshotReader.h"
#include "engine/signal/reader/SnapshotBatchReader.h"
#include "state/MarketStateView.h"

namespace trading_engine::signal {

class MarketStateViewSnapshotReader final
    : public IMarketSnapshotReader,
      public ISnapshotBatchReader {
public:
    explicit MarketStateViewSnapshotReader(
        const trading_engine::state::MarketStateView& view
    ) noexcept;

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

private:
    const trading_engine::state::MarketStateView& view_;
};

}  // namespace trading_engine::signal
