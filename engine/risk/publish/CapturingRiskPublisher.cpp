#include "engine/risk/publish/CapturingRiskPublisher.h"

namespace trading_engine::risk {

void CapturingRiskPublisher::publish_decision(
    const RiskDecision& decision,
    const RiskAuditTrace& trace
) {
    decisions_.push_back({decision, trace});
}

const std::vector<PublishedRiskDecision>& CapturingRiskPublisher::decisions()
    const noexcept {
    return decisions_;
}

void CapturingRiskPublisherFast::publish_decision(
    const RiskDecision& decision,
    const RiskAuditTrace& trace
) {
    PublishedRiskDecisionFast record;
    record.status = decision.status;
    record.reject_reason = decision.reject_reason;
    record.decision_id = decision.decision_id;
    record.intent_id = decision.intent_id != 0 ? decision.intent_id :
                                                trace.intent_id;
    record.bundle_id = decision.bundle_id != 0 ? decision.bundle_id :
                                                trace.bundle_id;
    record.policy_version = decision.policy_version;
    record.policy_hash = decision.policy_hash;
    record.audit_step_count = trace.lite.step_count;
    record.audit_steps = trace.lite.steps;
    decisions_.push_back(record);
}

void CapturingRiskPublisherFast::reserve(std::size_t capacity) {
    decisions_.reserve(capacity);
}

const std::vector<PublishedRiskDecisionFast>&
CapturingRiskPublisherFast::decisions() const noexcept {
    return decisions_;
}

}  // namespace trading_engine::risk
