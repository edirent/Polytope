#pragma once

#include "engine/execution/core/ExecutionContext.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/risk/public/ApprovedIntent.h"

namespace trading_engine::execution {

class OrderBuilder {
public:
    [[nodiscard]] OrderPlan build(
        const risk::ApprovedIntent& approved_intent,
        const ExecutionContext& context
    ) const;
};

}  // namespace trading_engine::execution
