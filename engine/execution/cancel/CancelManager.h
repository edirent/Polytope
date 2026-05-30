#pragma once

#include "engine/execution/adapter/IExecutionAdapter.h"
#include "engine/execution/public/ExecutionConfig.h"
#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/execution/state/FillTracker.h"

#include <string>
#include <vector>

namespace trading_engine::execution {

struct CancelWorkflowResult {
    bool ok = false;
    PlanId plan_id = 0;

    AdapterResultCode code = AdapterResultCode::AdapterError;
    std::vector<ExecutionReport> reports;

    std::string error;
};

class CancelManager {
public:
    explicit CancelManager(IExecutionAdapter* adapter = nullptr);

    [[nodiscard]] CancelWorkflowResult cancel_open_orders(
        const OrderPlan& plan,
        const PlanFillState& fill_state,
        const ExecutionConfig& config,
        std::uint64_t now_ns = 0
    );

    [[nodiscard]] ExecutionReport cancel(
        PlanId plan_id,
        ChildOrderId child_order_id
    );

private:
    IExecutionAdapter* adapter_ = nullptr;
};

}  // namespace trading_engine::execution
