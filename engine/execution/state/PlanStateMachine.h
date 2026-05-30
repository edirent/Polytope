#pragma once

#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/ExecutionTypes.h"

namespace trading_engine::execution {

class PlanStateMachine {
public:
    [[nodiscard]] bool can_transition(
        PlanStatus current,
        PlanStatus next
    ) const noexcept;

    [[nodiscard]] PlanStatus transition(
        PlanStatus current,
        PlanStatus next
    ) const noexcept;

    [[nodiscard]] PlanStatus apply(
        PlanStatus current,
        const ExecutionReport& report
    ) const noexcept;

    [[nodiscard]] static bool is_terminal(PlanStatus status) noexcept;
};

}  // namespace trading_engine::execution
