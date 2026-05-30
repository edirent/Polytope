#include "engine/execution/state/PartialFillPolicy.h"

namespace trading_engine::execution {

PartialFillAction PartialFillPolicy::decide(
    const PlanFillState& fill_state
) const noexcept {
    if (!fill_state.any_filled || fill_state.all_filled) {
        return PartialFillAction::None;
    }

    if (fill_state.child_count > 1) {
        return PartialFillAction::HedgeRequired;
    }

    if (fill_state.any_partial) {
        return PartialFillAction::CancelRemaining;
    }

    return PartialFillAction::MarkPartiallyFilled;
}

bool PartialFillPolicy::should_cancel_remaining(
    const FillTrackerState& fill_state
) const noexcept {
    return fill_state.filled_lots > 0;
}

}  // namespace trading_engine::execution
