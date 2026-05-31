#pragma once

#include "engine/signal/pricing/CostResult.h"
#include "engine/signal/reader/MarketSnapshotReader.h"
#include "engine/signal/reader/SnapshotBatchReader.h"
#include "engine/signal/scan/BundleRuntimePlan.h"

#include <vector>

namespace trading_engine::signal {

class VWAPPrecheck {
public:
    [[nodiscard]] CostResult price_bundle(
        const CandidateBundle& bundle,
        const std::vector<MarketStateSnapshot>& snapshots
    ) const;

    [[nodiscard]] CostResult price_runtime_plan(
        const BundleRuntimePlan& plan,
        const SnapshotBatchReadResult& snapshots
    ) const;

    [[nodiscard]] CostResult price_runtime_plan(
        const BundleRuntimePlan& plan,
        const DepthReadResult& depth
    ) const;
};

}  // namespace trading_engine::signal
