#include "engine/execution/core/ExecutionWorkflow.h"

namespace trading_engine::execution {

ExecutionResult ExecutionWorkflow::run(
    const risk::ApprovedIntent& approved_intent,
    const ExecutionContext&
) const {
    ExecutionResult result;
    result.ok = approved_intent.valid();
    result.status = result.ok ? PlanStatus::Created : PlanStatus::Rejected;
    if (!result.ok) {
        result.error = "approved intent is invalid";
    }
    return result;
}

}  // namespace trading_engine::execution
