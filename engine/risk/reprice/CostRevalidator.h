#pragma once

#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/risk/public/RiskResult.h"
#include "engine/risk/reprice/CostRevalidationResult.h"
#include "engine/risk/reprice/FeeRevalidator.h"
#include "engine/risk/reprice/LatencyRevalidator.h"
#include "engine/risk/reprice/SlippageRevalidator.h"
#include "engine/risk/reprice/VWAPRevalidator.h"
#include "engine/state/view/MarketDepthView.h"

#include <cstddef>
#include <vector>

namespace trading_engine::risk {

class CostRevalidator {
public:
    [[nodiscard]] CostRevalidationResult revalidate(
        const signal::OpportunityIntent& intent,
        const std::vector<state::MarketStateSnapshot>& snapshots,
        const RiskPolicySnapshot& policy
    ) const;

    [[nodiscard]] CostRevalidationResult revalidate(
        const signal::OpportunityIntent& intent,
        const std::vector<state::MarketStateSnapshot>& snapshots,
        const RiskPolicySnapshot& policy,
        RiskStageTimings* timings
    ) const;

    [[nodiscard]] CostRevalidationResult revalidate(
        const signal::OpportunityIntent& intent,
        const state::MarketStateSnapshot* snapshots,
        std::size_t snapshot_count,
        const RiskPolicySnapshot& policy,
        std::uint64_t now_ns,
        std::uint64_t snapshot_version_hash,
        RiskStageTimings* timings
    ) const;

    [[nodiscard]] CostRevalidationResult revalidate(
        const signal::OpportunityIntent& intent,
        const state::MarketDepthView* depth_views,
        std::size_t depth_view_count,
        const RiskPolicySnapshot& policy,
        std::uint64_t now_ns,
        std::uint64_t snapshot_version_hash,
        RiskStageTimings* timings
    ) const;

    [[nodiscard]] CostRevalidationResult revalidate(
        const signal::OpportunityIntent& intent,
        const std::vector<state::MarketStateSnapshot>& snapshots,
        const RiskPolicySnapshot& policy,
        std::uint64_t now_ns,
        RiskStageTimings* timings
    ) const;

    [[nodiscard]] CostRevalidationResult revalidate(
        const signal::OpportunityIntent& intent,
        const std::vector<state::MarketStateSnapshot>& snapshots,
        const RiskPolicySnapshot& policy,
        std::uint64_t now_ns,
        std::uint64_t snapshot_version_hash,
        RiskStageTimings* timings
    ) const;

private:
    VWAPRevalidator vwap_;
    FeeRevalidator fee_;
    SlippageRevalidator slippage_;
    LatencyRevalidator latency_;
};

}  // namespace trading_engine::risk
