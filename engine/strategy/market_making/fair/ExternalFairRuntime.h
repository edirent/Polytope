#pragma once

#include "engine/strategy/market_making/fair/BarrierTouchFairModel.h"
#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"
#include "engine/strategy/market_making/fair/SpotOracle.h"
#include "engine/strategy/market_making/fair/VolProvider.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

class ExternalFairRuntime {
public:
    ExternalFairRuntime(
        const SpotOracle& spot_oracle,
        const VolProvider& vol_provider
    );

    [[nodiscard]] ExternalFairResult compute(
        const ExternalFairMarketSpec& spec,
        std::int64_t now_ms
    ) const;

private:
    const SpotOracle& spot_oracle_;
    const VolProvider& vol_provider_;
    BarrierTouchFairModel barrier_touch_model_;
};

}  // namespace trading_engine::strategy::market_making
