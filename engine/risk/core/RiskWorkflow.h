#pragma once

#include "engine/risk/core/RiskEngine.h"

#include <vector>

namespace trading_engine::risk {

struct RiskWorkflowResult {
    std::vector<RiskPipelineResult> evaluations;
    RiskResult aggregate;
};

[[nodiscard]] RiskWorkflowResult run_risk_workflow(
    RiskEngine* engine,
    const std::vector<signal::OpportunityIntent>& intents,
    const RiskEvaluationContext& context
);

}  // namespace trading_engine::risk
