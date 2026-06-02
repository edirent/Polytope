#pragma once

#include "engine/strategy/market_making/public/MarketMakingConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace trading_engine::strategy::market_making {

class InventorySkewModel {
public:
    [[nodiscard]] std::int64_t compute(
        const MarketMakingConfig& config,
        std::int64_t current_position_lots
    ) const noexcept {
        if (config.max_inventory_lots <= 0 ||
            config.max_inventory_skew_tick == 0) {
            return 0;
        }
        const auto delta =
            current_position_lots - config.target_position_lots;
        const auto ratio = std::clamp(
            static_cast<long double>(delta) /
                static_cast<long double>(config.max_inventory_lots),
            -1.0L,
            1.0L
        );
        return static_cast<std::int64_t>(
            std::llround(ratio * config.max_inventory_skew_tick)
        );
    }
};

}  // namespace trading_engine::strategy::market_making
