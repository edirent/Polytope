#pragma once

#include "engine/risk/publish/RiskDecisionPublisher.h"

#include <vector>

namespace trading_engine::risk {

struct PublishedRiskDecision {
    RiskDecision decision;
    RiskAuditTrace trace;
};

class CapturingRiskPublisher final : public IRiskDecisionPublisher {
public:
    void publish_decision(
        const RiskDecision& decision,
        const RiskAuditTrace& trace
    ) override;

    [[nodiscard]] const std::vector<PublishedRiskDecision>& decisions()
        const noexcept;

private:
    std::vector<PublishedRiskDecision> decisions_;
};

}  // namespace trading_engine::risk
