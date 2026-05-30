#include "engine/signal/pricing/VWAPPrecheck.h"

#include "engine/signal/pricing/PriceVectorBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace trading_engine::signal {

namespace {

[[nodiscard]] CostResult failure(CostFailureReason reason) {
    CostResult result;
    result.failure_reason = reason;
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

struct LegCapacity {
    FillSimulationLeg leg;
    std::int64_t best_price_tick = 0;
};

[[nodiscard]] bool checked_add_i64(
    std::int64_t value,
    std::int64_t* total
) noexcept {
    const auto next =
        static_cast<__int128>(*total) + static_cast<__int128>(value);
    if (next > std::numeric_limits<std::int64_t>::max() ||
        next < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *total = static_cast<std::int64_t>(next);
    return true;
}

[[nodiscard]] bool checked_mul_i64(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) * static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max() ||
        value < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] std::int64_t ceil_div_positive(
    std::int64_t numerator,
    std::int64_t denominator
) noexcept {
    if (denominator <= 0) {
        return 0;
    }
    const auto value =
        (static_cast<__int128>(numerator) + denominator - 1) /
        static_cast<__int128>(denominator);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] CostFailureReason calculate_buy_capacity(
    const PriceLeg& leg,
    LegCapacity* out
) {
    if (leg.target_qty_lots <= 0) {
        return CostFailureReason::InvalidQuantity;
    }
    if (leg.executable_side != ExecutableBookSide::Asks || !leg.snapshot) {
        return CostFailureReason::InvalidLeg;
    }
    const auto& snapshot = *leg.snapshot;
    if (!snapshot.has_ask || snapshot.ask_count == 0) {
        return CostFailureReason::MissingBookSide;
    }

    out->leg.asset_id = leg.asset_id;
    out->leg.requested_qty_lots = leg.target_qty_lots;

    const auto level_count = std::min<std::uint32_t>(
        snapshot.ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );

    for (std::uint32_t i = 0; i < level_count; ++i) {
        const auto& level = snapshot.asks[i];
        const auto available = level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }
        if (out->best_price_tick == 0) {
            out->best_price_tick = level.price_tick;
        }
        if (!checked_add_i64(available, &out->leg.executable_qty_lots)) {
            return CostFailureReason::InvalidLeg;
        }
    }

    out->leg.enough_depth =
        out->leg.executable_qty_lots >= out->leg.requested_qty_lots;
    return CostFailureReason::None;
}

[[nodiscard]] CostFailureReason price_buy_leg_for_quantity(
    const PriceLeg& leg,
    std::int64_t quantity_lots,
    FillSimulationLeg* out
) {
    if (quantity_lots <= 0) {
        return CostFailureReason::InvalidQuantity;
    }
    if (!leg.snapshot) {
        return CostFailureReason::InvalidLeg;
    }

    const auto& snapshot = *leg.snapshot;
    const auto level_count = std::min<std::uint32_t>(
        snapshot.ask_count,
        trading_engine::state::kMaxSnapshotDepth
    );

    std::int64_t remaining = quantity_lots;
    std::int64_t leg_cost = 0;
    std::int64_t worst_price_tick = 0;
    std::int64_t best_price_tick = 0;
    for (std::uint32_t i = 0; i < level_count && remaining > 0; ++i) {
        const auto& level = snapshot.asks[i];
        const auto available = level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }

        if (best_price_tick == 0) {
            best_price_tick = level.price_tick;
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

    out->asset_id = leg.asset_id;
    out->total_cost_tick = leg_cost;
    out->vwap_price_tick = leg_cost / quantity_lots;
    out->worst_price_tick = worst_price_tick;
    out->enough_depth = true;
    out->book_age_ns = 0;
    if (best_price_tick > 0) {
        // Slippage is derived by the caller from worst minus best.
    }
    return CostFailureReason::None;
}

}  // namespace

CostResult VWAPPrecheck::price_bundle(
    const CandidateBundle& bundle,
    const std::vector<MarketStateSnapshot>& snapshots
) const {
    const auto vector_result = build_price_vector(bundle, snapshots);
    if (!vector_result.ok) {
        return failure(vector_result.failure_reason);
    }

    CostResult result;
    std::vector<LegCapacity> capacities;
    capacities.reserve(bundle.leg_count);
    result.legs.reserve(bundle.leg_count);

    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& leg = vector_result.legs[i];

        LegCapacity capacity;
        const auto reason = calculate_buy_capacity(leg, &capacity);
        if (reason != CostFailureReason::None) {
            result.failure_reason = reason;
            result.legs.push_back(capacity.leg);
            return result;
        }
        result.legs.push_back(capacity.leg);
        capacities.push_back(capacity);
    }

    result.bundle_qty = std::numeric_limits<std::int64_t>::max();
    for (const auto& leg : result.legs) {
        if (!leg.enough_depth || leg.requested_qty_lots <= 0) {
            result.failure_reason = CostFailureReason::InsufficientDepth;
            result.executable = false;
            return result;
        }
        result.bundle_qty = std::min(
            result.bundle_qty,
            leg.executable_qty_lots / leg.requested_qty_lots
        );
    }

    if (result.bundle_qty <= 0 ||
        result.bundle_qty == std::numeric_limits<std::int64_t>::max()) {
        result.failure_reason = CostFailureReason::InsufficientDepth;
        result.executable = false;
        return result;
    }

    std::int64_t total_bundle_lots = 0;
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& price_leg = vector_result.legs[i];

        std::int64_t planned_qty_lots = 0;
        if (!checked_mul_i64(
                price_leg.target_qty_lots,
                result.bundle_qty,
                &planned_qty_lots
            )) {
            return failure(CostFailureReason::InvalidQuantity);
        }

        auto priced = result.legs[i];
        const auto reason = price_buy_leg_for_quantity(
            price_leg,
            planned_qty_lots,
            &priced
        );
        if (reason != CostFailureReason::None) {
            result.failure_reason = reason;
            result.executable = false;
            result.legs[i] = priced;
            return result;
        }

        result.max_leg_slippage_tick = std::max(
            result.max_leg_slippage_tick,
            priced.worst_price_tick - capacities[i].best_price_tick
        );
        if (!checked_add_i64(priced.total_cost_tick, &result.total_cost_tick)) {
            return failure(CostFailureReason::InvalidLeg);
        }
        if (!checked_add_i64(planned_qty_lots, &total_bundle_lots)) {
            return failure(CostFailureReason::InvalidQuantity);
        }
        result.legs[i] = priced;
    }

    if (total_bundle_lots <= 0) {
        return failure(CostFailureReason::InvalidQuantity);
    }

    result.avg_cost_tick = ceil_div_positive(
        result.total_cost_tick,
        result.bundle_qty
    );
    result.executable = true;
    result.failure_reason = CostFailureReason::None;
    return result;
}

}  // namespace trading_engine::signal
