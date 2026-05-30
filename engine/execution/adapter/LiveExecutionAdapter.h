#pragma once

#include "engine/execution/adapter/IExecutionAdapter.h"

namespace trading_engine::execution {

class LiveExecutionAdapter final : public IExecutionAdapter {
public:
    [[nodiscard]] AdapterSubmitResult submit_plan(
        const OrderPlan& plan,
        const ExecutionContext& context
    ) override;

    [[nodiscard]] std::vector<ExecutionReport> poll_reports() override;

    [[nodiscard]] AdapterCancelResult cancel_plan(
        std::uint64_t plan_id
    ) override;
};

}  // namespace trading_engine::execution
