#pragma once

#include "engine/signal/public/SignalConfig.h"
#include "engine/signal/public/SignalTypes.h"
#include "engine/signal/reader/SnapshotVersion.h"
#include "state/MarketStateSnapshot.h"

#include <span>
#include <string>

namespace trading_engine::signal {

struct SnapshotConsistencyResult {
    bool ok = false;
    IntentStatus rejection_status = IntentStatus::CandidateOnly;
    std::string error;
    SnapshotVersion version;
};

class SnapshotConsistencyGuard {
public:
    [[nodiscard]] SnapshotConsistencyResult check(
        std::span<const trading_engine::state::MarketStateSnapshot> snapshots,
        std::uint64_t now_ns,
        const SignalConfig& config
    ) const;
};

}  // namespace trading_engine::signal
