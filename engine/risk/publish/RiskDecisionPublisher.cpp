#include "engine/risk/publish/RiskDecisionPublisher.h"

#include "engine/risk/core/RiskContext.h"

namespace trading_engine::risk {

void IRiskDecisionPublisher::publish_result(const RiskPipelineResult& result) {
    publish_decision(result.decision, result.audit_trace);
}

}  // namespace trading_engine::risk
