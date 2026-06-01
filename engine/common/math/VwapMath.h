#pragma once

#include "engine/common/math/FixedPointMath.h"
#include "state/StateTypes.h"
#include "state/book/DepthPrefix.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace trading_engine::common::math {

struct VwapMathResult {
    bool ok = false;
    std::int64_t total_cost_tick = 0;
    std::int64_t vwap_tick = 0;
    std::int64_t worst_price_tick = 0;
    std::int64_t executable_qty_lots = 0;
};

[[nodiscard]] inline std::int64_t price_level_size_lots(
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

[[nodiscard]] inline VwapMathResult buy_vwap_linear(
    const trading_engine::state::PriceLevel* asks,
    std::uint16_t ask_count,
    std::int64_t qty_lots
) noexcept {
    VwapMathResult result;
    if (asks == nullptr || ask_count == 0 || qty_lots <= 0) {
        return result;
    }

    std::int64_t remaining = qty_lots;
    for (std::uint16_t i = 0; i < ask_count; ++i) {
        const auto& level = asks[i];
        const auto available = price_level_size_lots(level);
        if (level.price_tick <= 0 || available <= 0) {
            continue;
        }

        if (!checked_add_i64(
                result.executable_qty_lots,
                available,
                &result.executable_qty_lots
            )) {
            return {};
        }

        if (remaining <= 0) {
            continue;
        }

        const auto take = std::min(remaining, available);
        std::int64_t level_cost = 0;
        if (!checked_mul_i64(level.price_tick, take, &level_cost) ||
            !checked_add_i64(
                result.total_cost_tick,
                level_cost,
                &result.total_cost_tick
            )) {
            return {};
        }
        result.worst_price_tick =
            std::max(result.worst_price_tick, level.price_tick);
        remaining -= take;
    }

    if (remaining > 0) {
        result.ok = false;
        return result;
    }

    result.ok = true;
    result.vwap_tick = result.total_cost_tick / qty_lots;
    return result;
}

[[nodiscard]] inline VwapMathResult buy_vwap_prefix(
    const trading_engine::state::DepthPrefix& prefix,
    const trading_engine::state::PriceLevel* asks,
    std::uint16_t ask_count,
    std::int64_t qty_lots
) noexcept {
    VwapMathResult result;
    if (asks == nullptr || qty_lots <= 0 || prefix.ask_count == 0 ||
        ask_count == 0 || prefix.ask_count > ask_count) {
        return result;
    }

    result.executable_qty_lots = prefix.ask_cum_qty[prefix.ask_count - 1U];
    if (result.executable_qty_lots < qty_lots) {
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
    const auto price_tick = asks[level_index].price_tick;
    if (remaining < 0 || price_tick <= 0) {
        return result;
    }

    std::int64_t partial_cost = 0;
    if (!checked_mul_i64(price_tick, remaining, &partial_cost) ||
        !checked_add_i64(cost_before, partial_cost, &result.total_cost_tick)) {
        return {};
    }

    result.ok = true;
    result.vwap_tick = result.total_cost_tick / qty_lots;
    result.worst_price_tick = price_tick;
    return result;
}

}  // namespace trading_engine::common::math
