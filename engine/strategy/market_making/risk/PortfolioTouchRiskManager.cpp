#include "engine/strategy/market_making/risk/PortfolioTouchRiskManager.h"

#include <cstdlib>

namespace trading_engine::strategy::market_making {

PortfolioTouchRiskOutput PortfolioTouchRiskManager::evaluate(
    const PortfolioTouchRiskInput& input
) const noexcept {
    PortfolioTouchRiskOutput output;
    const auto projected =
        input.current_canonical_yes_position_lots +
        input.proposed_canonical_yes_delta_lots;
    output.projected_total_touch_yes_lots = std::llabs(projected);
    if (input.event_type == ExternalFairEventType::UpTouch) {
        output.projected_upside_touch_lots = std::llabs(projected);
    } else if (input.event_type == ExternalFairEventType::DownTouch) {
        output.projected_downside_touch_lots = std::llabs(projected);
    }

    if (output.projected_total_touch_yes_lots >
        input.max_total_touch_yes_lots) {
        output.ok = false;
        output.reason = "max_total_touch_yes_lots";
        return output;
    }
    if (output.projected_upside_touch_lots >
        input.max_upside_touch_lots) {
        output.ok = false;
        output.reason = "max_upside_touch_lots";
        return output;
    }
    if (output.projected_downside_touch_lots >
        input.max_downside_touch_lots) {
        output.ok = false;
        output.reason = "max_downside_touch_lots";
        return output;
    }
    output.reason = "ok";
    return output;
}

}  // namespace trading_engine::strategy::market_making
