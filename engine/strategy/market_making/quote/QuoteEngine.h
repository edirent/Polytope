#pragma once

#include "engine/reward/public/RewardTypes.h"
#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/fair/FairValueModel.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"
#include "engine/strategy/market_making/quote/QuoteSizeModel.h"
#include "engine/strategy/market_making/quote/SpreadModel.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

struct QuoteBuildInput {
    std::string market_id;
    std::string asset_id;
    std::uint32_t market_index = 0;
    std::uint32_t asset_index = 0;

    const state::MarketDepthView* depth = nullptr;
    const MarketMakingConfig* config = nullptr;
    FairValueResult fair_value;
    SpreadResult spread;
    QuoteSizeResult size;
    const reward::RewardConfigSnapshot* reward_config = nullptr;
    std::int64_t inventory_skew_tick = 0;
    std::int64_t current_position_lots = 0;
    std::uint64_t now_ns = 0;
    QuoteIntentType intent_type = QuoteIntentType::PlaceQuote;
};

struct QuoteBuildResult {
    bool ok = false;
    QuoteIntent quote;
    std::string reason;
};

class QuoteEngine {
public:
    [[nodiscard]] QuoteBuildResult build(
        const QuoteBuildInput& input
    ) const;
};

}  // namespace trading_engine::strategy::market_making
