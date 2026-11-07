#pragma once

#include "engine/strategy/market_making/products/up_down_5m/UpDown5mMarketContext.h"
#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making::products::up_down_5m {

struct UpDown5mFairOutput {
    bool ok = false;
    std::int64_t up_fair_tick = 0;
    std::int64_t down_fair_tick = 0;
    std::int64_t asset_fair_tick = 0;
    double up_probability = 0.0;
    std::int64_t z_score_bps = 0;
    std::uint64_t tau_ms = 0;
    std::string reject_reason;
};

class UpDown5mFairProvider {
public:
    [[nodiscard]] UpDown5mFairOutput compute(
        const UpDown5mMarketContext& context
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making::products::up_down_5m
