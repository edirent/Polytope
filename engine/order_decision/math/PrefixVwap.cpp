#include "engine/order_decision/math/PrefixVwap.h"

#include "engine/common/math/FixedPointMath.h"

namespace trading_engine::order_decision {

PrefixVwapResult buy_vwap_from_prefix(
    const trading_engine::state::MarketDepthView& view,
    std::int64_t qty_lots
) noexcept {
    PrefixVwapResult result;
    result.qty_lots = qty_lots;

    const auto& prefix = view.prefix;
    if (qty_lots <= 0 || view.ask_count == 0 || prefix.ask_count == 0 ||
        prefix.ask_count > view.ask_count) {
        return result;
    }
    if (prefix.ask_cum_qty[prefix.ask_count - 1U] < qty_lots) {
        return result;
    }

    std::uint16_t level_index = 0;
    for (; level_index < prefix.ask_count; ++level_index) {
        if (prefix.ask_cum_qty[level_index] >= qty_lots) {
            break;
        }
    }
    if (level_index >= prefix.ask_count) {
        return result;
    }

    const auto qty_before =
        level_index == 0 ? 0 : prefix.ask_cum_qty[level_index - 1U];
    const auto cost_before =
        level_index == 0 ? 0 : prefix.ask_cum_cost[level_index - 1U];
    const auto remaining = qty_lots - qty_before;
    const auto price_tick = view.asks[level_index].price_tick;
    if (remaining < 0 || price_tick <= 0) {
        return result;
    }

    std::int64_t partial_cost = 0;
    if (!trading_engine::common::math::checked_mul_i64(
            price_tick,
            remaining,
            &partial_cost
        )) {
        return result;
    }

    std::int64_t total_cost = 0;
    if (!trading_engine::common::math::checked_add_i64(
            cost_before,
            partial_cost,
            &total_cost
        )) {
        return result;
    }

    result.ok = true;
    result.total_cost_tick = total_cost;
    result.vwap_tick = total_cost / qty_lots;
    result.worst_price_tick = price_tick;
    result.level_index = level_index;
    return result;
}

}  // namespace trading_engine::order_decision
