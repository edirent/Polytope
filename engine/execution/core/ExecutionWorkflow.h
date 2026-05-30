#pragma once

#include "engine/execution/core/ExecutionContext.h"
#include "engine/execution/public/ExecutionResult.h"
#include "engine/risk/public/ApprovedIntent.h"

namespace trading_engine::execution {

class ExecutionWorkflow {
public:
    [[nodiscard]] ExecutionResult run(
        const risk::ApprovedIntent& approved_intent,
        const ExecutionContext& context
    ) const;
};

}  // namespace trading_engine::execution
