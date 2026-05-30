#include "engine/execution/state/PlanStateMachine.h"

namespace trading_engine::execution {

bool PlanStateMachine::is_terminal(PlanStatus status) noexcept {
    switch (status) {
        case PlanStatus::Filled:
        case PlanStatus::Cancelled:
        case PlanStatus::Failed:
        case PlanStatus::Expired:
            return true;
        case PlanStatus::Created:
        case PlanStatus::Planned:
        case PlanStatus::Sent:
        case PlanStatus::Acked:
        case PlanStatus::PartiallyFilled:
        case PlanStatus::HedgeRequired:
        case PlanStatus::CancelRequested:
            return false;
    }
    return true;
}

bool PlanStateMachine::can_transition(
    PlanStatus current,
    PlanStatus next
) const noexcept {
    if (current == next) {
        return true;
    }
    if (is_terminal(current)) {
        return false;
    }
    if (next == PlanStatus::Failed || next == PlanStatus::Expired) {
        return true;
    }

    switch (current) {
        case PlanStatus::Created:
            return next == PlanStatus::Planned;
        case PlanStatus::Planned:
            return next == PlanStatus::Sent;
        case PlanStatus::Sent:
            return next == PlanStatus::Acked;
        case PlanStatus::Acked:
            return next == PlanStatus::PartiallyFilled ||
                   next == PlanStatus::Filled;
        case PlanStatus::PartiallyFilled:
            return next == PlanStatus::HedgeRequired ||
                   next == PlanStatus::CancelRequested;
        case PlanStatus::CancelRequested:
            return next == PlanStatus::Cancelled;
        case PlanStatus::HedgeRequired:
            return next == PlanStatus::CancelRequested;
        case PlanStatus::Filled:
        case PlanStatus::Cancelled:
        case PlanStatus::Failed:
        case PlanStatus::Expired:
            return false;
    }

    return false;
}

PlanStatus PlanStateMachine::transition(
    PlanStatus current,
    PlanStatus next
) const noexcept {
    return can_transition(current, next) ? next : current;
}

PlanStatus PlanStateMachine::apply(
    PlanStatus current,
    const ExecutionReport& report
) const noexcept {
    PlanStatus next = current;
    switch (report.status) {
        case ChildOrderStatus::Created:
            return current;
        case ChildOrderStatus::Planned:
            next = PlanStatus::Planned;
            break;
        case ChildOrderStatus::Sent:
            next = PlanStatus::Sent;
            break;
        case ChildOrderStatus::Acked:
            next = PlanStatus::Acked;
            break;
        case ChildOrderStatus::PartiallyFilled:
            next = PlanStatus::PartiallyFilled;
            break;
        case ChildOrderStatus::Filled:
            next = PlanStatus::Filled;
            break;
        case ChildOrderStatus::CancelRequested:
            next = PlanStatus::CancelRequested;
            break;
        case ChildOrderStatus::Cancelled:
            next = PlanStatus::Cancelled;
            break;
        case ChildOrderStatus::Failed:
            next = PlanStatus::Failed;
            break;
        case ChildOrderStatus::Expired:
            next = PlanStatus::Expired;
            break;
    }
    return transition(current, next);
}

}  // namespace trading_engine::execution
