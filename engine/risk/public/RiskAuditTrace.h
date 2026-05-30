#pragma once

#include "engine/risk/public/RiskDecision.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::risk {

struct RiskAuditStep {
    std::string guard_name;
    bool pass = false;
    RiskDecisionType rejection = RiskDecisionType::RejectInternalError;
    std::string reason;
};

struct RiskAuditTrace {
    std::uint64_t trace_id = 0;
    std::uint64_t decision_id = 0;
    std::uint64_t intent_id = 0;
    std::uint64_t bundle_id = 0;

    std::uint64_t policy_version = 0;
    std::uint64_t policy_hash = 0;

    RiskDecision decision;
    std::vector<RiskAuditStep> steps;
    std::vector<std::string> evidence;
};

}  // namespace trading_engine::risk
