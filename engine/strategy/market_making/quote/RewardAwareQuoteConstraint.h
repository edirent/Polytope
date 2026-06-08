#pragma once

#include "engine/reward/public/RewardTypes.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"

namespace trading_engine::strategy::market_making {

class RewardAwareQuoteConstraint {
public:
    [[nodiscard]] bool apply(
        QuoteIntent* quote,
        const MarketMakingConfig& config,
        const reward::RewardConfigSnapshot* reward_config
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making
