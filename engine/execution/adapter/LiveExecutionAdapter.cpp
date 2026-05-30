#include "engine/execution/adapter/LiveExecutionAdapter.h"

namespace trading_engine::execution {

#ifndef EXECUTION_ENABLE_LIVE
#define EXECUTION_ENABLE_LIVE 0
#endif

AdapterSubmitResult LiveExecutionAdapter::submit_plan(
    const OrderPlan& plan,
    const ExecutionContext&
) {
#if EXECUTION_ENABLE_LIVE
    return {
        .ok = false,
        .plan_id = plan.plan_id,
        .status = PlanStatus::Rejected,
        .code = AdapterResultCode::LiveExecutionDisabled,
        .error = "live execution adapter has no live implementation in v0"
    };
#else
    return {
        .ok = false,
        .plan_id = plan.plan_id,
        .status = PlanStatus::Rejected,
        .code = AdapterResultCode::LiveExecutionDisabled,
        .error = "live execution adapter is disabled"
    };
#endif
}

std::vector<ExecutionReport> LiveExecutionAdapter::poll_reports() {
    return {};
}

AdapterCancelResult LiveExecutionAdapter::cancel_plan(
    std::uint64_t plan_id
) {
    return {
        .ok = false,
        .plan_id = plan_id,
        .code = AdapterResultCode::LiveExecutionDisabled,
        .error = "live execution adapter is disabled"
    };
}

}  // namespace trading_engine::execution
