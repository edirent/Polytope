#pragma once

#include "engine/risk/public/RiskTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::risk {

struct RiskDecision {
    RiskDecisionStatus status = RiskDecisionStatus::Rejected;
    RiskRejectReason reject_reason = RiskRejectReason::NotEvaluated;

    std::uint64_t decision_id = 0;
    std::uint64_t intent_id = 0;
    std::uint64_t bundle_id = 0;
    std::uint64_t idempotency_hash = 0;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t constraint_hash = 0;
    std::uint64_t bundle_hash = 0;
    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t policy_version = 0;
    std::uint64_t policy_hash = 0;

    std::string reject_detail;

    [[nodiscard]] bool approved() const noexcept {
        return status == RiskDecisionStatus::Approved &&
               reject_reason == RiskRejectReason::None;
    }

    [[nodiscard]] bool rejected() const noexcept {
        return !approved();
    }
};

[[nodiscard]] inline RiskDecision make_rejected_decision(
    RiskRejectReason reason,
    std::string detail = {}
) {
    RiskDecision decision;
    decision.status = RiskDecisionStatus::Rejected;
    decision.reject_reason = reason;
    decision.reject_detail = std::move(detail);
    return decision;
}

[[nodiscard]] inline RiskDecision make_approved_decision(
    std::uint64_t policy_version,
    std::uint64_t policy_hash
) noexcept {
    RiskDecision decision;
    decision.status = RiskDecisionStatus::Approved;
    decision.reject_reason = RiskRejectReason::None;
    decision.policy_version = policy_version;
    decision.policy_hash = policy_hash;
    return decision;
}

}  // namespace trading_engine::risk
