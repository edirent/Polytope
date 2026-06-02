#pragma once

#include "engine/strategy/market_making/public/MarketMakingConfig.h"

namespace trading_engine::strategy::market_making {

struct QuotePriceClampResult {
    bool ok = false;
    std::int64_t bid_tick = 0;
    std::int64_t ask_tick = 0;
};

[[nodiscard]] inline QuotePriceClampResult clamp_quote_prices(
    std::int64_t bid_tick,
    std::int64_t ask_tick,
    const MarketMakingConfig& config
) noexcept {
    QuotePriceClampResult result;
    result.bid_tick =
        clamp_tick(bid_tick, config.min_price_tick, config.max_price_tick);
    result.ask_tick =
        clamp_tick(ask_tick, config.min_price_tick, config.max_price_tick);
    result.ok = result.bid_tick > 0 && result.ask_tick > 0 &&
                result.bid_tick < result.ask_tick;
    return result;
}

}  // namespace trading_engine::strategy::market_making
