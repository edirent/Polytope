#pragma once

#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/ExecutionTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::execution {

struct ExecutionResult {
    bool ok = false;
    PlanId plan_id = 0;
    PlanStatus status = PlanStatus::Created;

    std::uint64_t child_orders_submitted = 0;
    std::uint64_t child_orders_rejected = 0;

    std::string error;
};

enum class AdapterResultCode : std::uint8_t {
    Ok,
    LiveExecutionDisabled,
    AdapterError,
    MissingAdapter
};

struct AdapterSubmitResult {
    bool ok = false;
    PlanId plan_id = 0;
    PlanStatus status = PlanStatus::Created;

    std::uint64_t child_orders_submitted = 0;
    std::uint64_t child_orders_rejected = 0;

    AdapterResultCode code = AdapterResultCode::AdapterError;
    std::string error;
};

struct AdapterCancelResult {
    bool ok = false;
    PlanId plan_id = 0;

    AdapterResultCode code = AdapterResultCode::AdapterError;
    std::string error;
};

struct CancelResult {
    bool ok = false;
    PlanId plan_id = 0;

    AdapterResultCode code = AdapterResultCode::AdapterError;
    std::vector<ExecutionReport> reports;

    std::string error;
};

}  // namespace trading_engine::execution
