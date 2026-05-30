#include "engine/execution/state/OrderStateMachine.h"

namespace trading_engine::execution {

bool OrderStateMachine::is_terminal(ChildOrderStatus status) noexcept {
    switch (status) {
        case ChildOrderStatus::Filled:
        case ChildOrderStatus::Cancelled:
        case ChildOrderStatus::Failed:
        case ChildOrderStatus::Expired:
            return true;
        case ChildOrderStatus::Created:
        case ChildOrderStatus::Planned:
        case ChildOrderStatus::Sent:
        case ChildOrderStatus::Acked:
        case ChildOrderStatus::PartiallyFilled:
        case ChildOrderStatus::CancelRequested:
            return false;
    }
    return true;
}

bool OrderStateMachine::can_transition(
    ChildOrderStatus current,
    ChildOrderStatus next
) const noexcept {
    if (current == next) {
        return true;
    }
    if (is_terminal(current)) {
        return false;
    }
    if (next == ChildOrderStatus::Failed ||
        next == ChildOrderStatus::Expired) {
        return true;
    }

    switch (current) {
        case ChildOrderStatus::Created:
            return next == ChildOrderStatus::Planned;
        case ChildOrderStatus::Planned:
            return next == ChildOrderStatus::Sent;
        case ChildOrderStatus::Sent:
            return next == ChildOrderStatus::Acked;
        case ChildOrderStatus::Acked:
            return next == ChildOrderStatus::PartiallyFilled ||
                   next == ChildOrderStatus::Filled;
        case ChildOrderStatus::PartiallyFilled:
            return next == ChildOrderStatus::Filled ||
                   next == ChildOrderStatus::CancelRequested;
        case ChildOrderStatus::CancelRequested:
            return next == ChildOrderStatus::Cancelled;
        case ChildOrderStatus::Filled:
        case ChildOrderStatus::Cancelled:
        case ChildOrderStatus::Failed:
        case ChildOrderStatus::Expired:
            return false;
    }

    return false;
}

ChildOrderStatus OrderStateMachine::transition(
    ChildOrderStatus current,
    ChildOrderStatus next
) const noexcept {
    return can_transition(current, next) ? next : current;
}

ChildOrderStatus OrderStateMachine::apply(
    ChildOrderStatus current,
    const ExecutionReport& report
) const noexcept {
    return transition(current, report.status);
}

}  // namespace trading_engine::execution
