#pragma once

#include "engine/execution/core/ExecutionContext.h"
#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/ExecutionResult.h"
#include "engine/execution/public/OrderPlan.h"

#include <cstdint>
#include <vector>

namespace trading_engine::execution {

class IExecutionAdapter {
public:
    virtual ~IExecutionAdapter() = default;

    [[nodiscard]] virtual AdapterSubmitResult submit_plan(
        const OrderPlan& plan,
        const ExecutionContext& context
    ) = 0;

    [[nodiscard]] virtual std::vector<ExecutionReport> poll_reports() = 0;

    [[nodiscard]] virtual AdapterCancelResult cancel_plan(
        std::uint64_t plan_id
    ) = 0;
};

}  // namespace trading_engine::execution
