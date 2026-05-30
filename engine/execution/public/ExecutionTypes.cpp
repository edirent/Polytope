#include "engine/execution/public/ExecutionTypes.h"

namespace trading_engine::execution {

const char* to_string(ExecutionMode mode) noexcept {
    switch (mode) {
        case ExecutionMode::Paper:
            return "Paper";
        case ExecutionMode::Sandbox:
            return "Sandbox";
        case ExecutionMode::Live:
            return "Live";
    }
    return "Unknown";
}

const char* to_string(OrderSide side) noexcept {
    switch (side) {
        case OrderSide::Buy:
            return "Buy";
        case OrderSide::Sell:
            return "Sell";
    }
    return "Unknown";
}

const char* to_string(ChildOrderStatus status) noexcept {
    switch (status) {
        case ChildOrderStatus::Created:
            return "Created";
        case ChildOrderStatus::Planned:
            return "Planned";
        case ChildOrderStatus::Sent:
            return "Sent";
        case ChildOrderStatus::Acked:
            return "Acked";
        case ChildOrderStatus::PartiallyFilled:
            return "PartiallyFilled";
        case ChildOrderStatus::Filled:
            return "Filled";
        case ChildOrderStatus::CancelRequested:
            return "CancelRequested";
        case ChildOrderStatus::Cancelled:
            return "Cancelled";
        case ChildOrderStatus::Failed:
            return "Failed";
        case ChildOrderStatus::Expired:
            return "Expired";
    }
    return "Unknown";
}

const char* to_string(PlanStatus status) noexcept {
    switch (status) {
        case PlanStatus::Created:
            return "Created";
        case PlanStatus::Planned:
            return "Planned";
        case PlanStatus::Sent:
            return "Sent";
        case PlanStatus::Acked:
            return "Acked";
        case PlanStatus::PartiallyFilled:
            return "PartiallyFilled";
        case PlanStatus::HedgeRequired:
            return "HedgeRequired";
        case PlanStatus::Filled:
            return "Filled";
        case PlanStatus::CancelRequested:
            return "CancelRequested";
        case PlanStatus::Cancelled:
            return "Cancelled";
        case PlanStatus::Failed:
            return "Failed";
        case PlanStatus::Expired:
            return "Expired";
    }
    return "Unknown";
}

}  // namespace trading_engine::execution
