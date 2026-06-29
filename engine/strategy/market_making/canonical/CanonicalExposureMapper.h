#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

[[nodiscard]] inline std::int64_t to_canonical_yes_exposure(
    OutcomeSide asset_side,
    std::int64_t asset_position_lots
) noexcept {
    return asset_side == OutcomeSide::Yes
        ? asset_position_lots
        : -asset_position_lots;
}

[[nodiscard]] inline std::int64_t canonical_yes_target_to_asset_position(
    OutcomeSide asset_side,
    std::int64_t canonical_yes_target_lots
) noexcept {
    return asset_side == OutcomeSide::Yes
        ? canonical_yes_target_lots
        : -canonical_yes_target_lots;
}

[[nodiscard]] inline std::int64_t canonical_yes_delta_for_asset_fill(
    OutcomeSide asset_side,
    bool buy_asset,
    std::int64_t fill_qty_lots
) noexcept {
    const auto asset_delta = buy_asset ? fill_qty_lots : -fill_qty_lots;
    return to_canonical_yes_exposure(asset_side, asset_delta);
}

}  // namespace trading_engine::strategy::market_making
