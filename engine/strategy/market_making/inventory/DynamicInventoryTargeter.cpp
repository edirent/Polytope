#include "engine/strategy/market_making/inventory/DynamicInventoryTargeter.h"

#include <algorithm>
#include <cstdlib>

namespace trading_engine::strategy::market_making {

InventoryTargetOutput DynamicInventoryTargeter::compute(
    const InventoryTargetInput& input
) const {
    InventoryTargetOutput output;
    output.min_canonical_yes_lots = -25;
    output.max_canonical_yes_lots =
        input.event_type == ExternalFairEventType::UpTouch ? 15 : 25;

    const auto edge_tick =
        input.canonical_yes_tradable_fair_tick -
        input.canonical_yes_market_mid_tick;
    const auto abs_edge = std::llabs(edge_tick);
    std::int64_t target_abs = 0;
    if (abs_edge < 1 * kTicksPerCent) {
        output.reason = "edge_lt_1c";
    } else if (abs_edge < 2 * kTicksPerCent) {
        target_abs = 5;
        output.reason = "edge_1c_2c";
    } else if (abs_edge < 4 * kTicksPerCent) {
        target_abs = 15;
        output.reason = "edge_2c_4c";
    } else if (abs_edge < 8 * kTicksPerCent) {
        target_abs = 25;
        output.reason = "edge_4c_8c";
    } else if (input.confidence_bps >= 8'000) {
        target_abs = 25;
        output.reason = "edge_gt_8c_high_confidence";
    } else {
        target_abs = 0;
        output.reason = "edge_gt_8c_low_confidence";
    }

    if (input.event_type == ExternalFairEventType::UpTouch) {
        target_abs = std::min<std::int64_t>(target_abs, 15);
    }
    const auto signed_target = edge_tick > 0 ? target_abs : -target_abs;
    output.target_canonical_yes_lots = std::clamp(
        signed_target,
        output.min_canonical_yes_lots,
        output.max_canonical_yes_lots
    );
    output.inventory_skew_tick = std::clamp<std::int64_t>(
        (input.current_canonical_yes_position_lots -
         output.target_canonical_yes_lots) *
            1'000,
        -75'000,
        75'000
    );
    return output;
}

}  // namespace trading_engine::strategy::market_making
