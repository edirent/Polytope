#pragma once

#include "engine/risk/public/RiskAuditTrace.h"
#include "engine/risk/public/RiskDecision.h"

namespace trading_engine::risk {

struct RiskPipelineResult;

class IRiskDecisionPublisher {
public:
    virtual ~IRiskDecisionPublisher() = default;

    virtual void publish_decision(
        const RiskDecision& decision,
        const RiskAuditTrace& trace
    ) = 0;

    virtual void publish_result(const RiskPipelineResult& result);
};

}  // namespace trading_engine::risk
