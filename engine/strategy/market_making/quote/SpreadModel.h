#pragma once

#include "engine/strategy/market_making/public/MarketMakingConfig.h"

#include <algorithm>
#include <cstdint>

namespace trading_engine::strategy::market_making {

struct SpreadResult {
    std::int64_t half_spread_tick = 0;
    std::int64_t fee_buffer_tick = 0;
    std::int64_t latency_buffer_tick = 0;
    std::int64_t volatility_buffer_tick = 0;
    std::int64_t liquidity_buffer_tick = 0;
    std::int64_t uncertainty_buffer_tick = 0;
};

class SpreadModel {
public:
    [[nodiscard]] SpreadResult compute(
        const MarketMakingConfig& config
    ) const noexcept {
        SpreadResult result;
        result.fee_buffer_tick = config.fee_buffer_tick;
        result.latency_buffer_tick = config.latency_buffer_tick;
        result.volatility_buffer_tick = config.volatility_buffer_tick;
        result.liquidity_buffer_tick = config.liquidity_buffer_tick;
        result.uncertainty_buffer_tick = config.uncertainty_buffer_tick;
        result.half_spread_tick = std::max({
            config.min_half_spread_tick,
            config.fee_buffer_tick,
            config.latency_buffer_tick,
            config.volatility_buffer_tick,
            config.liquidity_buffer_tick,
            config.uncertainty_buffer_tick
        });
        return result;
    }
};

}  // namespace trading_engine::strategy::market_making
