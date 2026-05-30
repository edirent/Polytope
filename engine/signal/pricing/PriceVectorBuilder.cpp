#include "engine/signal/pricing/PriceVectorBuilder.h"

#include <unordered_map>

namespace trading_engine::signal {

PriceVectorResult PriceVectorBuilder::build(
    const CandidateBundle& bundle,
    const std::vector<MarketStateSnapshot>& snapshots
) const {
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
        if (leg.quantity_lots <= 0) {
            result.failure_reason = CostFailureReason::InvalidQuantity;
            result.failed_leg_index = i;
            return result;
        }

        auto& price_leg = result.legs[i];
        price_leg.asset_id = leg.asset_id;
        price_leg.market_id = leg.market_id;
        price_leg.side = leg.side;
        price_leg.target_qty_lots = leg.quantity_lots;
        price_leg.executable_side = side_resolver_.resolve(leg.side);
        if (price_leg.executable_side == ExecutableBookSide::Unsupported) {
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

        price_leg.snapshot = it->second;
        result.leg_count = static_cast<std::uint16_t>(i + 1U);
    }

    result.ok = true;
    return result;
}

PriceVectorResult build_price_vector(
    const CandidateBundle& bundle,
    const std::vector<MarketStateSnapshot>& snapshots
) {
    return PriceVectorBuilder{}.build(bundle, snapshots);
}

}  // namespace trading_engine::signal
