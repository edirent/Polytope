#include "engine/risk/core/RiskWorkflow.h"

#include <utility>

namespace trading_engine::risk {

RiskWorkflowResult run_risk_workflow(
    RiskEngine* engine,
    const std::vector<signal::OpportunityIntent>& intents,
    const RiskEvaluationContext& context
) {
    RiskWorkflowResult out;
    if (engine == nullptr) {
        return out;
    }

    out.evaluations.reserve(intents.size());
    for (const auto& intent : intents) {
        auto result = engine->evaluate(intent, context);
        out.aggregate.intents_evaluated += result.result.intents_evaluated;
        out.aggregate.intents_approved += result.result.intents_approved;
        out.aggregate.intents_rejected += result.result.intents_rejected;
        out.aggregate.rejected_kill_switch +=
            result.result.rejected_kill_switch;
        out.aggregate.rejected_low_edge += result.result.rejected_low_edge;
        out.aggregate.rejected_cost_limit += result.result.rejected_cost_limit;
        out.aggregate.rejected_exposure_limit +=
            result.result.rejected_exposure_limit;
        out.aggregate.rejected_inventory_limit +=
            result.result.rejected_inventory_limit;
        out.aggregate.rejected_stale_or_expired +=
            result.result.rejected_stale_or_expired;
        out.aggregate.rejected_drift_or_slippage +=
            result.result.rejected_drift_or_slippage;
        out.aggregate.rejected_pending_or_rate_limit +=
            result.result.rejected_pending_or_rate_limit;
        out.aggregate.rejected_bad_market_state +=
            result.result.rejected_bad_market_state;
        out.aggregate.rejected_snapshot_freshness +=
            result.result.rejected_snapshot_freshness;
        out.aggregate.rejected_partial_fill_risk +=
            result.result.rejected_partial_fill_risk;
        out.aggregate.output_hash ^= result.output_hash;
        out.evaluations.push_back(std::move(result));
    }

    return out;
}

}  // namespace trading_engine::risk
