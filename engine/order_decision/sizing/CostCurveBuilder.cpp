#include "engine/order_decision/sizing/CostCurveBuilder.h"

#include "engine/common/math/FixedPointMath.h"
#include "engine/common/math/VwapMath.h"

namespace trading_engine::order_decision {

CostCurveBuildResult CostCurveBuilder::build_buy_curve(
    const CostCurveBuildInput& input
) const {
    CostCurveBuildResult result;
    if (input.leg == nullptr || input.depth == nullptr) {
        result.reject_reason = OrderDecisionType::RejectInvalidBundle;
        result.error = "missing leg or depth";
        return result;
    }
    if (input.leg->side != Side::Buy) {
        result.reject_reason = OrderDecisionType::RejectUnsupportedSide;
        result.error = "SELL is unsupported in OrderDecision v1";
        return result;
    }
    if (input.depth->ask_count == 0) {
        result.reject_reason = OrderDecisionType::RejectNoDepth;
        result.error = "missing ask depth";
        return result;
    }

    auto& curve = result.curve;
    curve.market_id = input.leg->market_id;
    curve.asset_id = input.leg->asset_id;
    curve.asset_index = input.asset_index;
    curve.side = Side::Buy;

    std::int64_t cumulative_qty = 0;
    std::int64_t cumulative_cost = 0;
    for (std::uint16_t i = 0; i < input.depth->ask_count &&
                              curve.level_count < kMaxCostCurveLevels;
         ++i) {
        const auto& level = input.depth->asks[i];
        const auto size_lots =
            common::math::price_level_size_lots(level);
        if (level.price_tick <= 0 || size_lots <= 0) {
            continue;
        }

        std::int64_t level_cost = 0;
        if (!common::math::checked_mul_i64(
                level.price_tick,
                size_lots,
                &level_cost
            ) ||
            !common::math::checked_add_i64(
                cumulative_qty,
                size_lots,
                &cumulative_qty
            ) ||
            !common::math::checked_add_i64(
                cumulative_cost,
                level_cost,
                &cumulative_cost
            )) {
            result.reject_reason = OrderDecisionType::RejectInternalError;
            result.error = "cost curve overflow";
            return result;
        }

        auto& out_level = curve.levels[curve.level_count++];
        out_level.price_tick = level.price_tick;
        out_level.size_lots = size_lots;
        out_level.cumulative_qty_lots = cumulative_qty;
        out_level.cumulative_cost_tick = cumulative_cost;
    }

    curve.total_qty_lots = cumulative_qty;
    if (curve.level_count == 0 || curve.total_qty_lots <= 0) {
        result.reject_reason = OrderDecisionType::RejectNoDepth;
        result.error = "ask depth has no executable quantity";
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace trading_engine::order_decision
