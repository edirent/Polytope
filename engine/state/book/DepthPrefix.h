#pragma once

#include "state/StateTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace trading_engine::state {

struct DepthPrefix {
    std::uint16_t ask_count = 0;
    std::uint16_t bid_count = 0;

    std::array<std::int64_t, kMaxSnapshotDepth> ask_cum_qty{};
    std::array<std::int64_t, kMaxSnapshotDepth> ask_cum_cost{};

    std::array<std::int64_t, kMaxSnapshotDepth> bid_cum_qty{};
    std::array<std::int64_t, kMaxSnapshotDepth> bid_cum_proceeds{};
};

struct PrefixVwapResult {
    bool ok = false;

    std::int64_t qty_lots = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t vwap_tick = 0;
    std::int64_t worst_price_tick = 0;
};

[[nodiscard]] inline std::int64_t depth_prefix_level_size_lots(
    const PriceLevel& level
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

[[nodiscard]] inline bool depth_prefix_checked_add(
    std::int64_t lhs,
    std::int64_t rhs,
    std::int64_t* out
) noexcept {
    const auto value =
        static_cast<__int128>(lhs) + static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max() ||
        value < std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] inline bool depth_prefix_checked_mul(
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

[[nodiscard]] inline std::int64_t depth_prefix_saturating_add(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    std::int64_t out = 0;
    if (!depth_prefix_checked_add(lhs, rhs, &out)) {
        return rhs >= 0 ? std::numeric_limits<std::int64_t>::max()
                        : std::numeric_limits<std::int64_t>::min();
    }
    return out;
}

[[nodiscard]] inline std::int64_t depth_prefix_saturating_mul(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    std::int64_t out = 0;
    if (!depth_prefix_checked_mul(lhs, rhs, &out)) {
        const bool positive = (lhs >= 0 && rhs >= 0) || (lhs < 0 && rhs < 0);
        return positive ? std::numeric_limits<std::int64_t>::max()
                        : std::numeric_limits<std::int64_t>::min();
    }
    return out;
}

inline void build_depth_prefix(
    const std::array<PriceLevel, kMaxSnapshotDepth>& bids,
    std::uint16_t bid_count,
    const std::array<PriceLevel, kMaxSnapshotDepth>& asks,
    std::uint16_t ask_count,
    DepthPrefix* out
) noexcept {
    if (!out) {
        return;
    }

    *out = DepthPrefix{};
    out->bid_count = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(bid_count, kMaxSnapshotDepth)
    );
    out->ask_count = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(ask_count, kMaxSnapshotDepth)
    );

    std::int64_t cum_qty = 0;
    std::int64_t cum_cost = 0;
    for (std::uint16_t i = 0; i < out->ask_count; ++i) {
        const auto& level = asks[i];
        const auto available =
            level.price_tick > 0 ? depth_prefix_level_size_lots(level) : 0;
        const auto cost =
            available > 0
                ? depth_prefix_saturating_mul(level.price_tick, available)
                : 0;
        cum_qty = depth_prefix_saturating_add(cum_qty, available);
        cum_cost = depth_prefix_saturating_add(cum_cost, cost);
        out->ask_cum_qty[i] = cum_qty;
        out->ask_cum_cost[i] = cum_cost;
    }

    cum_qty = 0;
    std::int64_t cum_proceeds = 0;
    for (std::uint16_t i = 0; i < out->bid_count; ++i) {
        const auto& level = bids[i];
        const auto available =
            level.price_tick > 0 ? depth_prefix_level_size_lots(level) : 0;
        const auto proceeds =
            available > 0
                ? depth_prefix_saturating_mul(level.price_tick, available)
                : 0;
        cum_qty = depth_prefix_saturating_add(cum_qty, available);
        cum_proceeds = depth_prefix_saturating_add(cum_proceeds, proceeds);
        out->bid_cum_qty[i] = cum_qty;
        out->bid_cum_proceeds[i] = cum_proceeds;
    }
}

[[nodiscard]] inline std::int64_t ask_depth_from_prefix(
    const DepthPrefix& prefix
) noexcept {
    if (prefix.ask_count == 0) {
        return 0;
    }
    return prefix.ask_cum_qty[prefix.ask_count - 1U];
}

[[nodiscard]] inline std::int64_t bid_depth_from_prefix(
    const DepthPrefix& prefix
) noexcept {
    if (prefix.bid_count == 0) {
        return 0;
    }
    return prefix.bid_cum_qty[prefix.bid_count - 1U];
}

[[nodiscard]] inline std::int64_t best_ask_tick_from_prefix(
    const std::array<PriceLevel, kMaxSnapshotDepth>& asks,
    const DepthPrefix& prefix
) noexcept {
    std::int64_t previous_qty = 0;
    for (std::uint16_t i = 0; i < prefix.ask_count; ++i) {
        const auto current_qty = prefix.ask_cum_qty[i];
        if (current_qty > previous_qty && asks[i].price_tick > 0) {
            return asks[i].price_tick;
        }
        previous_qty = current_qty;
    }
    return 0;
}

[[nodiscard]] inline PrefixVwapResult buy_vwap_from_prefix(
    const std::array<PriceLevel, kMaxSnapshotDepth>& asks,
    const DepthPrefix& prefix,
    std::int64_t qty_lots
) noexcept {
    PrefixVwapResult result;
    result.qty_lots = qty_lots;

    if (qty_lots <= 0 || prefix.ask_count == 0) {
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
    const auto price_tick = asks[level_index].price_tick;
    if (remaining < 0 || price_tick <= 0) {
        return result;
    }

    std::int64_t partial_cost = 0;
    if (!depth_prefix_checked_mul(price_tick, remaining, &partial_cost)) {
        return result;
    }

    std::int64_t total_cost = 0;
    if (!depth_prefix_checked_add(cost_before, partial_cost, &total_cost)) {
        return result;
    }

    result.ok = true;
    result.total_cost_tick = total_cost;
    result.vwap_tick = total_cost / qty_lots;
    result.worst_price_tick = price_tick;
    return result;
}

}  // namespace trading_engine::state
