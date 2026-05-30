#pragma once

#include "engine/execution/state/FillTracker.h"

#include <cstdint>

namespace trading_engine::execution {

enum class PartialFillAction : std::uint8_t {
    None,
    MarkPartiallyFilled,
    CancelRemaining,
    HedgeRequired,
    FailPlan
};

class PartialFillPolicy {
public:
    [[nodiscard]] PartialFillAction decide(
        const PlanFillState& fill_state
    ) const noexcept;

    [[nodiscard]] bool should_cancel_remaining(
        const FillTrackerState& fill_state
    ) const noexcept;
};

}  // namespace trading_engine::execution
