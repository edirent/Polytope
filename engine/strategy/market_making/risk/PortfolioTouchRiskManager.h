#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

struct PortfolioTouchRiskInput {
    ExternalFairEventType event_type = ExternalFairEventType::Unknown;
    std::int64_t current_canonical_yes_position_lots = 0;
    std::int64_t proposed_canonical_yes_delta_lots = 0;
    std::int64_t max_total_touch_yes_lots = 75;
    std::int64_t max_upside_touch_lots = 25;
    std::int64_t max_downside_touch_lots = 50;
};

struct PortfolioTouchRiskOutput {
    bool ok = true;
    std::int64_t projected_total_touch_yes_lots = 0;
    std::int64_t projected_upside_touch_lots = 0;
    std::int64_t projected_downside_touch_lots = 0;
    std::string reason;
};

class PortfolioTouchRiskManager {
public:
    [[nodiscard]] PortfolioTouchRiskOutput evaluate(
        const PortfolioTouchRiskInput& input
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making
