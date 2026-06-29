#pragma once

#include "engine/strategy/market_making/canonical/CanonicalMarketState.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct MarketImpliedFairOutput {
    bool ok = false;
    std::int64_t canonical_yes_market_mid_tick = 0;
    double implied_vol = 0.0;
    std::int64_t implied_fair_tick = 0;
    int confidence_bps = 0;
};

class MarketImpliedFairModel {
public:
    [[nodiscard]] MarketImpliedFairOutput compute(
        const CanonicalMarketState& state
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making
