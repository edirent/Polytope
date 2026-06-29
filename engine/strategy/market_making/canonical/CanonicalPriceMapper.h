#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <algorithm>
#include <cstdint>

namespace trading_engine::strategy::market_making {

[[nodiscard]] inline std::int64_t clamp_price_tick(
    std::int64_t value,
    std::int64_t price_scale_tick
) noexcept {
    return std::max<std::int64_t>(
        0,
        std::min<std::int64_t>(price_scale_tick, value)
    );
}

[[nodiscard]] inline std::int64_t asset_to_canonical_yes_tick(
    OutcomeSide asset_side,
    std::int64_t asset_price_tick,
    std::int64_t price_scale_tick
) noexcept {
    const auto clamped = clamp_price_tick(asset_price_tick, price_scale_tick);
    return asset_side == OutcomeSide::Yes
        ? clamped
        : price_scale_tick - clamped;
}

[[nodiscard]] inline std::int64_t canonical_yes_to_asset_tick(
    OutcomeSide asset_side,
    std::int64_t canonical_yes_price_tick,
    std::int64_t price_scale_tick
) noexcept {
    const auto clamped =
        clamp_price_tick(canonical_yes_price_tick, price_scale_tick);
    return asset_side == OutcomeSide::Yes
        ? clamped
        : price_scale_tick - clamped;
}

struct CanonicalTopOfBook {
    std::int64_t bid_tick = 0;
    std::int64_t ask_tick = 0;
    std::int64_t mid_tick = 0;
    std::int64_t spread_tick = 0;
};

[[nodiscard]] inline CanonicalTopOfBook canonical_yes_top_of_book(
    OutcomeSide asset_side,
    std::int64_t asset_bid_tick,
    std::int64_t asset_ask_tick,
    std::int64_t price_scale_tick
) noexcept {
    CanonicalTopOfBook out;
    if (asset_bid_tick <= 0 || asset_ask_tick <= 0 ||
        asset_ask_tick < asset_bid_tick || price_scale_tick <= 0) {
        return out;
    }

    if (asset_side == OutcomeSide::Yes) {
        out.bid_tick = asset_bid_tick;
        out.ask_tick = asset_ask_tick;
    } else {
        out.bid_tick = price_scale_tick - asset_ask_tick;
        out.ask_tick = price_scale_tick - asset_bid_tick;
    }

    out.bid_tick = clamp_price_tick(out.bid_tick, price_scale_tick);
    out.ask_tick = clamp_price_tick(out.ask_tick, price_scale_tick);
    if (out.ask_tick < out.bid_tick) {
        out = CanonicalTopOfBook{};
        return out;
    }
    out.mid_tick = (out.bid_tick + out.ask_tick) / 2;
    out.spread_tick = out.ask_tick - out.bid_tick;
    return out;
}

}  // namespace trading_engine::strategy::market_making
