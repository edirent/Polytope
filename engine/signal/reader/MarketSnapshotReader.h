#pragma once

#include "engine/signal/public/OpportunityIntent.h"
#include "engine/signal/public/SignalConfig.h"
#include "engine/signal/reader/OracleArtifactReader.h"
#include "state/MarketStateSnapshot.h"

#include <string>
#include <vector>

namespace trading_engine::signal {

using MarketStateSnapshot = trading_engine::state::MarketStateSnapshot;

struct SnapshotReadResult {
    bool ok = false;

    std::string error;
    IntentStatus rejection_status = IntentStatus::CandidateOnly;

    std::vector<MarketStateSnapshot> snapshots;
};

class IMarketSnapshotReader {
public:
    [[nodiscard]] virtual SnapshotReadResult read_for_bundle(
        const CandidateBundle& bundle,
        const SignalConfig& config
    ) const = 0;

    virtual ~IMarketSnapshotReader() = default;
};

[[nodiscard]] SnapshotReadResult validate_bundle_snapshots(
    const CandidateBundle& bundle,
    const SignalConfig& config,
    const std::vector<MarketStateSnapshot>& snapshots
);

}  // namespace trading_engine::signal
