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

}  // namespace trading_engine::risk
