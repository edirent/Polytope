#pragma once

#include "engine/risk/public/RiskAuditTrace.h"
#include "engine/risk/public/RiskDecision.h"

namespace trading_engine::risk {

class IRiskDecisionPublisher {
public:
    virtual ~IRiskDecisionPublisher() = default;

    virtual void publish_decision(
        const RiskDecision& decision,
        const RiskAuditTrace& trace
    ) = 0;
};

}  // namespace trading_engine::risk
