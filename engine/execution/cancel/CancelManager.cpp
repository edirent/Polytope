#include "engine/execution/cancel/CancelManager.h"

namespace trading_engine::execution {

namespace {

bool terminal_status(ChildOrderStatus status) noexcept {
    return status == ChildOrderStatus::Filled ||
        status == ChildOrderStatus::Cancelled ||
        status == ChildOrderStatus::Failed ||
        status == ChildOrderStatus::Expired;
}

std::int64_t open_remaining_lots(
    const ChildOrder& order,
    const PlanFillState& fill_state
) noexcept {
    if (terminal_status(order.status)) {
        return 0;
    }

    for (std::uint16_t i = 0; i < fill_state.child_count; ++i) {
        const auto& child = fill_state.children[i];
        if (child.order_id != order.order_id) {
            continue;
        }
        return child.remaining_qty_lots;
    }

    return order.quantity_lots;
}

ExecutionReport cancel_report(
    const OrderPlan& plan,
    const ChildOrder& order,
    ChildOrderStatus status,
    std::int64_t remaining_lots,
    std::uint64_t now_ns
) {
    return {
        .plan_id = plan.plan_id,
        .child_order_id = order.order_id,
        .status = status,
        .remaining_lots = status == ChildOrderStatus::Cancelled ? 0 :
            remaining_lots,
        .event_ts_ns = now_ns
    };
}

}  // namespace

CancelManager::CancelManager(IExecutionAdapter* adapter) : adapter_(adapter) {}

CancelWorkflowResult CancelManager::cancel_open_orders(
    const OrderPlan& plan,
    const PlanFillState& fill_state,
    const ExecutionConfig& config,
    std::uint64_t now_ns
) {
    if (config.mode == ExecutionMode::Live) {
        if (!config.execution_enabled || !config.live_enabled) {
            return {
                .ok = false,
                .plan_id = plan.plan_id,
                .code = AdapterResultCode::LiveExecutionDisabled,
                .error = "live execution cancel is disabled"
            };
        }
        if (adapter_ == nullptr) {
            return {
                .ok = false,
                .plan_id = plan.plan_id,
                .code = AdapterResultCode::MissingAdapter,
                .error = "missing execution adapter"
            };
        }

        const auto cancel = adapter_->cancel_plan(plan.plan_id);
        return {
            .ok = cancel.ok,
            .plan_id = cancel.plan_id,
            .code = cancel.code,
            .error = cancel.error
        };
    }

    if (config.mode == ExecutionMode::Sandbox) {
        return {
            .ok = false,
            .plan_id = plan.plan_id,
            .code = AdapterResultCode::AdapterError,
            .error = "sandbox execution cancel is not wired"
        };
    }

    CancelWorkflowResult result;
    result.ok = true;
    result.plan_id = plan.plan_id;
    result.code = AdapterResultCode::Ok;

    for (std::uint16_t i = 0; i < plan.order_count; ++i) {
        const auto& order = plan.orders[i];
        const auto remaining_lots = open_remaining_lots(order, fill_state);
        if (remaining_lots <= 0) {
            continue;
        }

        result.reports.push_back(cancel_report(
            plan,
            order,
            ChildOrderStatus::CancelRequested,
            remaining_lots,
            now_ns
        ));
        result.reports.push_back(cancel_report(
            plan,
            order,
            ChildOrderStatus::Cancelled,
            remaining_lots,
            now_ns
        ));
    }

    return result;
}

ExecutionReport CancelManager::cancel(
    PlanId plan_id,
    ChildOrderId child_order_id
) {
    if (adapter_ == nullptr) {
        return {
            .plan_id = plan_id,
            .child_order_id = child_order_id,
            .status = ChildOrderStatus::Rejected,
            .reject_reason = "missing execution adapter"
        };
    }
    const auto result = adapter_->cancel_plan(plan_id);
    return {
        .plan_id = plan_id,
        .child_order_id = child_order_id,
        .status = result.ok ? ChildOrderStatus::Canceled :
                              ChildOrderStatus::Rejected,
        .reject_reason = result.error
    };
}

}  // namespace trading_engine::execution
