#pragma once

#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/ExecutionTypes.h"

namespace trading_engine::execution {

class OrderStateMachine {
public:
    [[nodiscard]] bool can_transition(
        ChildOrderStatus current,
        ChildOrderStatus next
    ) const noexcept;

    [[nodiscard]] ChildOrderStatus transition(
        ChildOrderStatus current,
        ChildOrderStatus next
    ) const noexcept;

    [[nodiscard]] ChildOrderStatus apply(
        ChildOrderStatus current,
        const ExecutionReport& report
    ) const noexcept;

    [[nodiscard]] static bool is_terminal(
        ChildOrderStatus status
    ) noexcept;
};

}  // namespace trading_engine::execution
