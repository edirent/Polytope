#include "engine/order_decision/math/CostCurve.h"

#include "engine/common/math/FixedPointMath.h"

namespace trading_engine::order_decision {

CostForQuantityResult cost_for_quantity(
    const CostCurve& curve,
    std::int64_t quantity_lots,
    CostForQuantityStats* stats
) noexcept {
    CostForQuantityResult result;
    result.quantity_lots = quantity_lots;
    if (stats != nullptr) {
        stats->depth_levels_scanned = 0;
    }
    if (quantity_lots <= 0 || curve.level_count == 0 ||
        quantity_lots > curve.total_qty_lots) {
        return result;
    }

    std::int64_t previous_qty = 0;
    std::int64_t previous_cost = 0;
    for (std::uint16_t i = 0; i < curve.level_count; ++i) {
        if (stats != nullptr) {
            ++stats->depth_levels_scanned;
        }
        const auto& level = curve.levels[i];
        if (level.cumulative_qty_lots < quantity_lots) {
            previous_qty = level.cumulative_qty_lots;
            previous_cost = level.cumulative_cost_tick;
            continue;
        }

        const auto remaining = quantity_lots - previous_qty;
        std::int64_t partial_cost = 0;
        if (!common::math::checked_mul_i64(
                remaining,
                level.price_tick,
                &partial_cost
            ) ||
            !common::math::checked_add_i64(
                previous_cost,
                partial_cost,
                &result.total_cost_tick
            )) {
            return {};
        }
        result.vwap_tick = result.total_cost_tick / quantity_lots;
        result.worst_price_tick = level.price_tick;
        result.ok = true;
        return result;
    }

    return result;
}

}  // namespace trading_engine::order_decision
