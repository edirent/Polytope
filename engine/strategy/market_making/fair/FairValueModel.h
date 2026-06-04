#pragma once

#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"
#include "engine/strategy/market_making/public/MarketMakingTypes.h"

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct FairValueResult {
    bool ok = false;
    std::int64_t fair_value_tick = 0;
    std::int64_t midpoint_tick = 0;
    std::int64_t microprice_tick = 0;
    std::int64_t vwap_mid_tick = 0;
    std::int64_t complement_midpoint_tick = 0;
    std::int64_t complement_implied_tick = 0;
    std::int64_t external_fair_value_tick = 0;
    std::int64_t book_spread_tick = 0;
    std::int64_t confidence_bps = 0;
    std::int64_t top_bid_qty_lots = 0;
    std::int64_t top_ask_qty_lots = 0;
    FairValueQuality quality = FairValueQuality::Disabled;
    FairValueSourceKind source = FairValueSourceKind::Mid;
};

class FairValueModel {
public:
    [[nodiscard]] FairValueResult compute(
        const state::MarketDepthView& depth,
        const MarketMakingConfig& config,
        std::uint64_t now_ns,
        const state::MarketDepthView* complement_depth = nullptr
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making
