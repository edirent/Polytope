#include "engine/execution/adapter/SandboxExecutionAdapter.h"

namespace trading_engine::execution {

AdapterSubmitResult SandboxExecutionAdapter::submit_plan(
    const OrderPlan& plan,
    const ExecutionContext&
) {
    return {
        .ok = false,
        .plan_id = plan.plan_id,
        .status = PlanStatus::Rejected,
        .code = AdapterResultCode::AdapterError,
        .error = "sandbox execution adapter is not wired"
    };
}

std::vector<ExecutionReport> SandboxExecutionAdapter::poll_reports() {
    return {};
}

AdapterCancelResult SandboxExecutionAdapter::cancel_plan(
    std::uint64_t plan_id
) {
    return {
        .ok = false,
        .plan_id = plan_id,
        .code = AdapterResultCode::AdapterError,
        .error = "sandbox execution adapter is not wired"
    };
}

}  // namespace trading_engine::execution
