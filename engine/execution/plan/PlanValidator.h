#pragma once

#include "engine/execution/public/ExecutionConfig.h"
#include "engine/execution/public/OrderPlan.h"

#include <string>

namespace trading_engine::execution {

struct ValidationResult {
    bool ok = false;
    std::string error;
};

using PlanValidationResult = ValidationResult;

class PlanValidator {
public:
    [[nodiscard]] ValidationResult validate(
        const OrderPlan& plan,
        const ExecutionConfig& config
    ) const;

    [[nodiscard]] PlanValidationResult validate(
        const OrderPlan& plan,
        bool require_reservation = true
    ) const;

    [[nodiscard]] PlanValidationResult validate_child_order(
        const ChildOrder& order
    ) const;
};

}  // namespace trading_engine::execution
