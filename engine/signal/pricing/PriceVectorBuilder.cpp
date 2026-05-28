#include "engine/signal/pricing/PriceVectorBuilder.h"

#include <unordered_map>

namespace trading_engine::signal {

PriceVectorResult build_price_vector(
    const CandidateBundle& bundle,
    const std::vector<MarketStateSnapshot>& snapshots
) {
    PriceVectorResult result;

    if (bundle.leg_count == 0 || bundle.leg_count > kMaxIntentLegs) {
        result.failure_reason = CostFailureReason::InvalidLeg;
        return result;
    }

    std::unordered_map<std::string, const MarketStateSnapshot*> by_asset;
    by_asset.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) {
        by_asset.emplace(snapshot.entity_id, &snapshot);
    }

    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& leg = bundle.legs[i];
        if (leg.asset_id.empty() || leg.market_id.empty()) {
            result.failure_reason = CostFailureReason::InvalidLeg;
            result.failed_leg_index = i;
            return result;
        }

        const auto it = by_asset.find(leg.asset_id);
        if (it == by_asset.end()) {
            result.failure_reason = CostFailureReason::MissingSnapshot;
            result.failed_leg_index = i;
            return result;
        }

        result.snapshots[i] = it->second;
    }

    result.ok = true;
    return result;
}

}  // namespace trading_engine::signal
