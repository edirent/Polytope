#include "engine/order_decision/limits/LimitPriceBuilder.h"

#include "engine/common/math/FixedPointMath.h"

namespace trading_engine::order_decision {

LimitPriceResult LimitPriceBuilder::build_buy_limit(
    const LimitPriceInput& input
) const {
    LimitPriceResult result;
    if (input.worst_consumed_price_tick <= 0) {
        result.reject_reason = OrderDecisionType::RejectNoDepth;
        return result;
    }

    std::int64_t protected_price = 0;
    if (!common::math::checked_add_i64(
            input.worst_consumed_price_tick,
            input.protection_buffer_tick,
            &protected_price
        )) {
        result.reject_reason = OrderDecisionType::RejectPriceProtection;
        return result;
    }

    auto max_allowed = input.configured_max_price_tick;
    if (input.leg_max_price_tick > 0 &&
        (max_allowed <= 0 || input.leg_max_price_tick < max_allowed)) {
        max_allowed = input.leg_max_price_tick;
    }
    if (max_allowed > 0 && protected_price > max_allowed) {
        result.reject_reason = OrderDecisionType::RejectPriceProtection;
        return result;
    }

    result.ok = true;
    result.limit_price_tick = protected_price;
    return result;
}

}  // namespace trading_engine::order_decision
