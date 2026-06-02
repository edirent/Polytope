#pragma once

#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"
#include "engine/strategy/market_making/state/ActiveQuoteState.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct QuoteRefreshDecision {
    bool should_cancel = false;
    bool should_replace = false;
    CancelReason reason = CancelReason::None;
};

class QuoteRefreshPolicy {
public:
    [[nodiscard]] QuoteRefreshDecision evaluate(
        const ActiveQuoteState* active,
        const QuoteIntent* candidate,
        const state::MarketDepthView& depth,
        const MarketMakingConfig& config,
        std::int64_t current_position_lots,
        std::uint64_t now_ns
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making
