#pragma once

#include "engine/signal/pricing/CostResult.h"
#include "engine/signal/reader/MarketSnapshotReader.h"

#include <vector>

namespace trading_engine::signal {

class VWAPPrecheck {
public:
    [[nodiscard]] CostResult price_bundle(
        const CandidateBundle& bundle,
        const std::vector<MarketStateSnapshot>& snapshots
    ) const;
};

}  // namespace trading_engine::signal
