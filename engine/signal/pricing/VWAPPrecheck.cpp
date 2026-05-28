#include "engine/signal/pricing/VWAPPrecheck.h"

#include "engine/signal/pricing/PriceVectorBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace trading_engine::signal {

namespace {

[[nodiscard]] CostResult failure(
    CostFailureReason reason,
    std::uint16_t leg_index
) {
    CostResult result;
    result.failure_reason = reason;
    result.failed_leg_index = leg_index;
    return result;
}

[[nodiscard]] std::int64_t level_size_lots(
    const trading_engine::state::PriceLevel& level
) noexcept {
    if (!std::isfinite(level.size) || level.size <= 0.0) {
        return 0;
    }
    if (level.size >=
        static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::floor(level.size));
}

[[nodiscard]] bool checked_add_cost(
    std::int64_t price_tick,
    std::int64_t quantity_lots,
    std::int64_t* total
) noexcept {
    const auto add =
        static_cast<__int128>(price_tick) *
        static_cast<__int128>(quantity_lots);
    const auto next = static_cast<__int128>(*total) + add;
    if (next > std::numeric_limits<std::int64_t>::max() ||
        next < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *total = static_cast<std::int64_t>(next);
    return true;
}

[[nodiscard]] CostFailureReason price_buy_leg(
    const trading_engine::oracle::BundleLeg& leg,
    const MarketStateSnapshot& snapshot,
    IntentLeg* priced_leg
) {
    if (leg.quantity_lots <= 0) {
        return CostFailureReason::InvalidQuantity;
    }
    if (!snapshot.has_ask || snapshot.ask_count == 0) {
        return CostFailureReason::MissingBookSide;
    }

    std::int64_t remaining = leg.quantity_lots;
    std::int64_t leg_cost = 0;
    std::int64_t worst_price_tick = 0;
    const auto level_count = std::min<std::uint32_t>(
        snapshot.ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );

    for (std::uint32_t i = 0; i < level_count && remaining > 0; ++i) {
        const auto& level = snapshot.asks[i];
        const auto available = level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }

        const auto take = std::min(remaining, available);
        if (!checked_add_cost(level.price_tick, take, &leg_cost)) {
            return CostFailureReason::InvalidLeg;
        }
        worst_price_tick = std::max(worst_price_tick, level.price_tick);
        remaining -= take;
    }

    if (remaining > 0) {
        return CostFailureReason::InsufficientDepth;
    }

    priced_leg->market_id = leg.market_id;
    priced_leg->asset_id = leg.asset_id;
    priced_leg->side = leg.side;
    priced_leg->quantity_lots = leg.quantity_lots;
    priced_leg->estimated_cost_tick = leg_cost;
    priced_leg->estimated_vwap_tick = leg_cost / leg.quantity_lots;
    priced_leg->worst_price_tick = worst_price_tick;
    priced_leg->enough_depth = true;
    return CostFailureReason::None;
}

}  // namespace

CostResult VWAPPrecheck::price_bundle(
    const CandidateBundle& bundle,
    const std::vector<MarketStateSnapshot>& snapshots
) const {
    const auto vector_result = build_price_vector(bundle, snapshots);
    if (!vector_result.ok) {
        return failure(
            vector_result.failure_reason,
            vector_result.failed_leg_index
        );
    }

    CostResult result;
    std::int64_t total_quantity_lots = 0;

    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& leg = bundle.legs[i];
        if (leg.side != Side::Buy) {
            return failure(CostFailureReason::InvalidLeg, i);
        }

        const auto reason = price_buy_leg(
            leg,
            *vector_result.snapshots[i],
            &result.priced_legs[i]
        );
        if (reason != CostFailureReason::None) {
            result.filled_leg_count = i;
            result.failure_reason = reason;
            result.failed_leg_index = i;
            return result;
        }

        if (!checked_add_cost(1, leg.quantity_lots, &total_quantity_lots)) {
            return failure(CostFailureReason::InvalidQuantity, i);
        }
        if (!checked_add_cost(
                1,
                result.priced_legs[i].estimated_cost_tick,
                &result.total_cost_tick
            )) {
            return failure(CostFailureReason::InvalidLeg, i);
        }
        result.worst_price_tick = std::max(
            result.worst_price_tick,
            result.priced_legs[i].worst_price_tick
        );
        result.filled_leg_count = static_cast<std::uint16_t>(i + 1U);
    }

    if (total_quantity_lots <= 0) {
        return failure(CostFailureReason::InvalidQuantity, 0);
    }

    result.bundle_vwap_tick = result.total_cost_tick / total_quantity_lots;
    result.enough_depth = true;
    result.failure_reason = CostFailureReason::None;
    return result;
}

}  // namespace trading_engine::signal
