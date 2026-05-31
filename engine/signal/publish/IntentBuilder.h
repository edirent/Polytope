#pragma once

#include "engine/signal/edge/EdgeBreakdown.h"
#include "engine/signal/pricing/CostResult.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/signal/reader/MarketSnapshotReader.h"

#include <cstdint>

namespace trading_engine::signal {

struct IntentBuildInput {
    const CandidateBundle* bundle = nullptr;
    const SnapshotReadResult* snapshot = nullptr;
    SnapshotVersion snapshot_version{};
    const CostResult* cost = nullptr;
    const EdgeBreakdown* edge = nullptr;

    std::uint64_t now_ns = 0;
    std::uint64_t ttl_ns = 0;

    std::uint64_t oracle_artifact_version = 0;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t constraint_hash = 0;
    std::uint64_t bundle_hash = 0;

    bool valid_under_settlement = true;
    bool passed_quality_gate = true;
};

class IntentBuilder {
public:
    [[nodiscard]] OpportunityIntent build(
        const IntentBuildInput& input
    ) const;
};

void materialize_intent_strings(OpportunityIntent* intent);

}  // namespace trading_engine::signal
