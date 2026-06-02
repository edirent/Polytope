#pragma once

#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"
#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct FairValueResult {
    bool ok = false;
    std::int64_t fair_value_tick = 0;
    FairValueQuality quality = FairValueQuality::Disabled;
    FairValueSourceKind source = FairValueSourceKind::Mid;
};

class FairValueModel {
public:
    [[nodiscard]] FairValueResult compute(
        const state::MarketDepthView& depth,
        const MarketMakingConfig& config,
        std::uint64_t now_ns
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making
