#pragma once

#include "engine/execution/core/ExecutionContext.h"
#include "engine/execution/public/ExecutionConfig.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/risk/public/ApprovedIntent.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>
#include <string>

namespace trading_engine::execution {

struct ExecutionApproval {
    std::uint64_t decision_id = 0;
    std::uint64_t reservation_id = 0;
    std::uint64_t bundle_id = 0;

    std::int64_t approved_bundle_qty = 0;

    std::string idempotency_key;
};

struct ApprovedIntentEnvelope {
    signal::OpportunityIntent source_intent;
    ExecutionApproval approval;
};

struct PlanResult {
    bool ok = false;
    OrderPlan plan;
    std::string error;
};

class ExecutionPlanner {
public:
    [[nodiscard]] PlanResult build_plan(
        const ApprovedIntentEnvelope& envelope,
        std::uint64_t now_ns,
        const ExecutionConfig& config
    ) const;

    [[nodiscard]] OrderPlan plan(
        const risk::ApprovedIntent& approved_intent,
        const ExecutionContext& context
    ) const;
};

}  // namespace trading_engine::execution
